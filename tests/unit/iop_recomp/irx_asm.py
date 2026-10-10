"""A tiny MIPS I assembler and IRX writer for the IOP recompiler tests.

Instructions are hand-encoded (field layouts from the MIPS R3000 manual); every word is checked
against rabbitizer's disassembly. Modules are linked at 0 with the relocations a real IRX has
(R_MIPS_26 for j/jal, HI16/LO16 for `la`, R_MIPS_32 for pointers in tables), so the recompiler's
and the runtime loader's relocation are exercised exactly as with the game's modules.
"""
import struct

import rabbitizer as rz

rz.config.pseudos_enablePseudos = False

ZERO, AT, V0, V1, A0, A1, A2, A3 = 0, 1, 2, 3, 4, 5, 6, 7
T0, T1, T2, T3, T4, T5, T6, T7 = 8, 9, 10, 11, 12, 13, 14, 15
S0, S1, S2, S3, S4, S5, S6, S7 = 16, 17, 18, 19, 20, 21, 22, 23
T8, T9, GP, SP, FP, RA = 24, 25, 28, 29, 30, 31

R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


class Asm:
    """Text and data of one module. Labels are module-relative addresses."""

    def __init__(self):
        self.text: list = []          # int words, or (kind, label, base word) to resolve
        self.data = bytearray()
        self.text_labels: dict[str, int] = {}
        self.data_labels: dict[str, int] = {}     # offsets into data
        self.data_ptrs: list[tuple[int, str]] = []  # (data offset, text/data label): R_MIPS_32
        self.mnemonics: list[str] = []

    # -- layout ---------------------------------------------------------------------------------
    def pc(self) -> int:
        return 4 * len(self.text)

    def label(self, name: str) -> None:
        self.text_labels[name] = self.pc()

    def emit(self, word, mnem: str) -> None:
        self.text.append(word)
        self.mnemonics.append(mnem)

    def dlabel(self, name: str, align: int = 4) -> None:
        while len(self.data) % align:
            self.data.append(0)
        self.data_labels[name] = len(self.data)

    def dbytes(self, b: bytes) -> None:
        self.data += b

    def dword(self, v: int) -> None:
        self.data += struct.pack("<I", v & 0xFFFFFFFF)

    def dptr(self, name: str) -> None:
        self.data_ptrs.append((len(self.data), name))
        self.data += b"\0\0\0\0"

    # -- encodings ------------------------------------------------------------------------------
    @staticmethod
    def R(funct, rs=0, rt=0, rd=0, sa=0): return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct
    @staticmethod
    def I(op, rs, rt, imm): return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)

    def r3(self, funct, mnem):
        def f(rd, rs, rt): self.emit(self.R(funct, rs, rt, rd), mnem)
        return f

    def __getattr__(self, name):
        three = {"addu": 0x21, "subu": 0x23, "and_": 0x24, "or_": 0x25, "xor": 0x26, "nor": 0x27,
                 "slt": 0x2A, "sltu": 0x2B, "add": 0x20, "sub": 0x22}
        if name in three:
            return self.r3(three[name], name.rstrip("_"))
        imm = {"addiu": 0x09, "slti": 0x0A, "sltiu": 0x0B, "andi": 0x0C, "ori": 0x0D, "xori": 0x0E, "addi": 0x08}
        if name in imm:
            return lambda rt, rs, v: self.emit(self.I(imm[name], rs, rt, v), name)
        mem = {"lb": 0x20, "lh": 0x21, "lwl": 0x22, "lw": 0x23, "lbu": 0x24, "lhu": 0x25, "lwr": 0x26,
               "sb": 0x28, "sh": 0x29, "swl": 0x2A, "sw": 0x2B, "swr": 0x2E}
        if name in mem:
            return lambda rt, off, base: self.emit(self.I(mem[name], base, rt, off), name)
        shift = {"sll": 0x00, "srl": 0x02, "sra": 0x03}
        if name in shift:
            return lambda rd, rt, sa: self.emit(self.R(shift[name], 0, rt, rd, sa), name)
        shiftv = {"sllv": 0x04, "srlv": 0x06, "srav": 0x07}
        if name in shiftv:
            return lambda rd, rt, rs: self.emit(self.R(shiftv[name], rs, rt, rd), name)
        md = {"mult": 0x18, "multu": 0x19, "div": 0x1A, "divu": 0x1B}
        if name in md:
            return lambda rs, rt: self.emit(self.R(md[name], rs, rt), name)
        raise AttributeError(name)

    def lui(self, rt, v): self.emit(self.I(0x0F, 0, rt, v), "lui")
    def mfhi(self, rd): self.emit(self.R(0x10, rd=rd), "mfhi")
    def mflo(self, rd): self.emit(self.R(0x12, rd=rd), "mflo")
    def mthi(self, rs): self.emit(self.R(0x11, rs), "mthi")
    def mtlo(self, rs): self.emit(self.R(0x13, rs), "mtlo")
    def jr(self, rs): self.emit(self.R(0x08, rs), "jr")
    def jalr(self, rs, rd=RA): self.emit(self.R(0x09, rs, 0, rd), "jalr")
    def nop(self): self.emit(0, "nop")
    def brk(self, code): self.emit(self.R(0x0D) | (code << 6), "break")

    def _branch(self, op, rs, rt, target, mnem):
        self.emit(("branch", target, self.I(op, rs, rt, 0)), mnem)

    def beq(self, rs, rt, t): self._branch(0x04, rs, rt, t, "beq")
    def bne(self, rs, rt, t): self._branch(0x05, rs, rt, t, "bne")
    def blez(self, rs, t): self._branch(0x06, rs, 0, t, "blez")
    def bgtz(self, rs, t): self._branch(0x07, rs, 0, t, "bgtz")
    def bltz(self, rs, t): self._branch(0x01, rs, 0x00, t, "bltz")
    def bgez(self, rs, t): self._branch(0x01, rs, 0x01, t, "bgez")
    def bltzal(self, rs, t): self._branch(0x01, rs, 0x10, t, "bltzal")
    def bgezal(self, rs, t): self._branch(0x01, rs, 0x11, t, "bgezal")
    def j(self, t): self.emit(("j26", t, 0x02 << 26), "j")
    def jal(self, t): self.emit(("j26", t, 0x03 << 26), "jal")

    def la(self, rt, name):
        """lui rt, %hi(name); addiu rt, rt, %lo(name) with HI16/LO16 relocations."""
        self.emit(("hi16", name, self.I(0x0F, 0, rt, 0)), "lui")
        self.emit(("lo16", name, self.I(0x09, rt, rt, 0)), "addiu")

    # -- import / export tables (end of .text, after all code) -------------------------------------
    def import_table(self, lib: str, nums: list[int]) -> dict[int, str]:
        """Returns {function number: stub label}; call stubs with jal(label)."""
        self.emit(0x41E00000, None); self.emit(0, None); self.emit(0x0101, None)
        name = lib.encode().ljust(8, b"\0")
        self.emit(struct.unpack("<I", name[:4])[0], None); self.emit(struct.unpack("<I", name[4:])[0], None)
        out = {}
        for n in nums:
            out[n] = f"__imp_{lib}_{n}"
            self.label(out[n])
            self.emit(0x03E00008, "jr")
            self.emit(0x24000000 | n, "addiu")
        self.emit(0, None)
        return out

    def export_table(self, lib: str, funcs: list[str]) -> None:
        self.emit(0x41C00000, None); self.emit(0, None); self.emit(0x0101, None)
        name = lib.encode().ljust(8, b"\0")
        self.emit(struct.unpack("<I", name[:4])[0], None); self.emit(struct.unpack("<I", name[4:])[0], None)
        for f in funcs:
            self.emit(("ptr", f, 0), None)
        self.emit(0, None)

    # -- linking --------------------------------------------------------------------------------
    def addr(self, name: str) -> int:
        if name in self.text_labels:
            return self.text_labels[name]
        return self.data_base() + self.data_labels[name]

    def data_base(self) -> int:
        return (self.pc() + 15) & ~15

    def link(self) -> tuple[bytes, list[tuple[int, int]], int]:
        """Image bytes (text + padding + data), relocations [(address, type)], text size."""
        words, relocs = [], []
        for k, w in enumerate(self.text):
            a = 4 * k
            if isinstance(w, int):
                words.append(w)
                continue
            kind, name, base = w
            t = self.addr(name)
            if kind == "branch":
                off = (t - (a + 4)) >> 2
                words.append(base | (off & 0xFFFF))
            elif kind == "j26":
                words.append(base | ((t >> 2) & 0x3FFFFFF))
                relocs.append((a, R_MIPS_26))
            elif kind == "hi16":
                words.append(base | (((t + 0x8000) >> 16) & 0xFFFF))
                relocs.append((a, R_MIPS_HI16))
            elif kind == "lo16":
                words.append(base | (t & 0xFFFF))
                relocs.append((a, R_MIPS_LO16))
            elif kind == "ptr":
                words.append(t)
                relocs.append((a, R_MIPS_32))
        for k, (w, m) in enumerate(zip(words, self.mnemonics)):
            if m is None:
                continue
            got = rz.Instruction(w, vram=4 * k, category=rz.InstrCategory.CPU).getOpcodeName()
            if got != m:
                raise ValueError(f"encoding at 0x{4 * k:X}: {w:08X} decodes as {got}, expected {m}")
        text = b"".join(struct.pack("<I", w) for w in words)
        text += b"\0" * (self.data_base() - len(text))
        data = bytearray(self.data)
        for off, name in self.data_ptrs:
            struct.pack_into("<I", data, off, self.addr(name))
            relocs.append((self.data_base() + off, R_MIPS_32))
        return text + bytes(data), relocs, self.pc()


def build_irx(name: str, asm: Asm, entry: str, bss: int = 0, version: int = 0x0101) -> bytes:
    """A relocatable IRX (ELF type 0xFF80) with .iopmod, .text, .data, .rel.text, .rel.data."""
    image, relocs, text_size = asm.link()
    text_end = asm.data_base()
    iopmod = struct.pack("<6IH", 0xFFFFFFFF, asm.addr(entry), 0, text_size, len(image) - text_size, bss, version)
    iopmod += name.encode() + b"\0"
    rel_text = b"".join(struct.pack("<II", a, t) for a, t in relocs if a < text_end)
    rel_data = b"".join(struct.pack("<II", a, t) for a, t in relocs if a >= text_end)
    shstr = b"\0.iopmod\0.text\0.rel.text\0.data\0.rel.data\0.shstrtab\0"
    def sn(s): return shstr.index(s.encode() + b"\0")

    off = 52 + 2 * 32
    iopmod_off = off; off += len(iopmod); off = (off + 15) & ~15
    image_off = off; off += len(image)
    rt_off = off; off += len(rel_text)
    rd_off = off; off += len(rel_data)
    str_off = off; off += len(shstr); off = (off + 3) & ~3
    sh_off = off
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (sn(".iopmod"), 0x70000080, 0, 0, iopmod_off, len(iopmod), 0, 0, 4, 0),
        (sn(".text"), 1, 6, 0, image_off, text_end, 0, 0, 16, 0),
        (sn(".rel.text"), 9, 0, 0, rt_off, len(rel_text), 0, 2, 4, 8),
        (sn(".data"), 1, 3, text_end, image_off + text_end, len(image) - text_end, 0, 0, 16, 0),
        (sn(".rel.data"), 9, 0, 0, rd_off, len(rel_data), 0, 4, 4, 8),
        (sn(".shstrtab"), 3, 0, 0, str_off, len(shstr), 0, 0, 1, 0),
    ]
    ehdr = b"\x7fELF\x01\x01\x01" + b"\0" * 9 + struct.pack(
        "<HHIIIIIHHHHHH", 0xFF80, 8, 1, asm.addr(entry), 52, sh_off, 0, 52, 32, 2, 40, len(sections), len(sections) - 1)
    phdrs = struct.pack("<8I", 0x70000080, iopmod_off, 0, 0, len(iopmod), 0, 4, 4)
    phdrs += struct.pack("<8I", 1, image_off, 0, 0, len(image), len(image) + bss, 7, 16)
    out = bytearray(ehdr + phdrs)
    out += iopmod
    out += b"\0" * (image_off - len(out))
    out += image + rel_text + rel_data + shstr
    out += b"\0" * (sh_off - len(out))
    for s in sections:
        out += struct.pack("<10I", *s)
    return bytes(out)
