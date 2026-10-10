"""Build a tiny PS2-style MIPS ELF from hand-encoded instructions, for testing the recompiler.

The program leaves known results in registers and returns from its entry point. EXPECTED lists
the correct low 64 bits of each register and EXPECTED128 the full 128 bits, per real R5900
semantics. Every encoding is checked against rabbitizer's disassembly when the file is built.
"""
import struct
import sys

BASE = 0x00100000
SUM = BASE + 0x200          # sum(n) = n + (n-1) + ... + 1
DATA = BASE + 0x300         # two float vectors for the VU0 tests

ZERO, V1, V0, A0, A1, A2, A3, T0, T1, T2, T3, T4, T5, T6, T7 = 0, 3, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
S0, S1, S2, S3, S4, S5, S6, S7, T9, SP, RA = 16, 17, 18, 19, 20, 21, 22, 23, 25, 29, 31


def r(funct, rs=0, rt=0, rd=0, sa=0, op=0): return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct
def i(op, rs, rt, imm): return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
def j(op, target): return (op << 26) | ((target >> 2) & 0x3FFFFFF)

# Each helper returns (word, expected mnemonic) so build() can verify the encoding.
def addiu(rt, rs, imm): return i(0x09, rs, rt, imm), "addiu"
def lui(rt, imm): return i(0x0F, 0, rt, imm), "lui"
def ori(rt, rs, imm): return i(0x0D, rs, rt, imm), "ori"
def lw(rt, off, base): return i(0x23, base, rt, off), "lw"
def sw(rt, off, base): return i(0x2B, base, rt, off), "sw"
def bne(rs, rt, off): return i(0x05, rs, rt, off), "bne"
def beql(rs, rt, off): return i(0x14, rs, rt, off), "beql"
def jal(target): return j(0x03, target), "jal"
def addu(rd, rs, rt): return r(0x21, rs, rt, rd), "addu"
def sll(rd, rt, sa): return r(0x00, 0, rt, rd, sa), "sll"
def dsll32(rd, rt, sa): return r(0x3C, 0, rt, rd, sa), "dsll32"
def div(rs, rt): return r(0x1A, rs, rt), "div"
def mult(rs, rt): return r(0x18, rs, rt), "mult"
def mflo(rd): return r(0x12, rd=rd), "mflo"
def mfhi(rd): return r(0x10, rd=rd), "mfhi"
def jr(rs): return r(0x08, rs), "jr"
NOP = (0, "nop")

# MMI (opcode 0x1C); the sub-op lives in the sa field
def pextlw(rd, rs, rt): return r(0x08, rs, rt, rd, 0x12, op=0x1C), "pextlw"
def prot3w(rd, rt): return r(0x09, 0, rt, rd, 0x1F, op=0x1C), "prot3w"
def qfsrv(rd, rs, rt): return r(0x28, rs, rt, rd, 0x1B, op=0x1C), "qfsrv"
def mtsab(rs, imm): return i(0x01, rs, 0x18, imm), "mtsab"

# VU0 macro mode (COP2, opcode 0x12)
XYZW, XYZ, X = 0xF, 0xE, 0x8
def cop2(dest, ft, fs, fd, funct): return (0x12 << 26) | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct
def cop2s2(dest, ft, fs, opc):  # "special2" ops: 11-bit opcode in bits 10..0
    return (0x12 << 26) | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | opc
def lqc2(vf, off, base): return i(0x36, base, vf, off), "lqc2"
def qmfc2(rt, vf): return (0x12 << 26) | (0x01 << 21) | (rt << 16) | (vf << 11), "qmfc2.ni"
def vadd(fd, fs, ft): return cop2(XYZW, ft, fs, fd, 0x28), "vadd"
def vmulx(fd, fs, ft): return cop2(XYZW, ft, fs, fd, 0x18), "vmulx"
def vopmula(fs, ft): return cop2s2(XYZ, ft, fs, 0x2FE), "vopmula"
def vopmsub(fd, fs, ft): return cop2(XYZ, ft, fs, fd, 0x2E), "vopmsub"
def vdiv(fs, fsf, ft, ftf): return (0x12 << 26) | (1 << 25) | (ftf << 23) | (fsf << 21) | (ft << 16) | (fs << 11) | (0x0E << 6) | 0x3C, "vdiv"
def vwaitq(): return cop2s2(0, 0, 0, 0x3BF), "vwaitq"
def vaddq(dest, fd, fs): return cop2(dest, 0, fs, fd, 0x20), "vaddq"

main = [
    addiu(SP, SP, -16),
    sw(RA, 0, SP),
    # s0 = sum(10) = 55: call + loop with a delay-slot branch
    addiu(A0, ZERO, 10),
    jal(SUM),
    NOP,
    addu(S0, V0, ZERO),
    # s1 = 0x7FFFFFFF << 1 -> 32-bit result sign-extends to 0xFFFFFFFFFFFFFFFE
    lui(T0, 0x7FFF), ori(T0, T0, 0xFFFF),
    sll(S1, T0, 1),
    # s2 = 1 << 63 via dsll32 (64-bit shift)
    addiu(T0, ZERO, 1),
    dsll32(S2, T0, 31),
    # s3 = -7 / 2 = -3 ; s4 = remainder -1
    addiu(T0, ZERO, -7), addiu(T1, ZERO, 2),
    div(T0, T1), mflo(S3), mfhi(S4),
    # s5 = 5 / 0 -> PS2 gives quotient -1 (no trap)
    addiu(T0, ZERO, 5),
    div(T0, ZERO), mflo(S5),
    # s6 = 0x10000 * 0x10000 -> hi = 1
    lui(T0, 1),
    mult(T0, T0), mfhi(S6),
    # s7: branch-likely NOT taken must skip its delay slot -> s7 stays 3
    addiu(S7, ZERO, 3),
    beql(ZERO, S7, 2),          # 0 == 3? no
    addiu(S7, ZERO, 99),        # delay slot: must NOT execute
    NOP,

    # --- MMI ---
    lui(T0, 0x1111), ori(T0, T0, 0x1111),
    lui(T1, 0x2222), ori(T1, T1, 0x2222),
    pextlw(A2, T0, T1),         # a2 = [w0=t1.w0, w1=t0.w0, w2=t1.w1, w3=t0.w1]
    prot3w(A3, A2),             # a3 = [a2.w1, a2.w2, a2.w0, a2.w3]
    mtsab(ZERO, 4),             # SA = 4 bytes
    qfsrv(V1, A3, A2),          # v1 = low 128 bits of (a3:a2) >> 32

    # --- VU0 ---
    lui(T9, DATA >> 16), ori(T9, T9, DATA & 0xFFFF),
    lqc2(1, 0, T9),             # vf1 = (1, 2, 3, 4)
    lqc2(2, 16, T9),            # vf2 = (5, 6, 7, 8)
    vadd(3, 1, 2),              # vf3 = (6, 8, 10, 12)
    qmfc2(T3, 3),
    vmulx(4, 1, 2),             # vf4 = vf1 * vf2.x = (5, 10, 15, 20)
    qmfc2(T4, 4),
    vopmula(1, 2),              # cross product vf1 x vf2 = (-4, 8, -4)
    vopmsub(5, 2, 1),
    qmfc2(T5, 5),
    vdiv(1, 3, 2, 0),           # Q = vf1.w / vf2.x = 0.8
    vwaitq(),
    vaddq(X, 6, 0),             # vf6.x = vf0.x + Q
    qmfc2(T6, 6),

    # --- memory map: hardware registers must not alias RAM; uncached mirror must ---
    addiu(T0, ZERO, 0x2000),
    addiu(T1, ZERO, 0x1234),
    sw(T1, 0, T0),              # RAM 0x2000 = 0x1234
    lui(T2, 0x1000),
    sw(T2, 0x2000, T2),         # write hardware register 0x10002000 (IPU), not RAM
    lui(T2, 0x2000),
    lw(T7, 0x2000, T2),         # read RAM 0x2000 through the uncached mirror 0x20002000

    lw(RA, 0, SP),
    addiu(SP, SP, 16),
    jr(RA),
    NOP,
]

sum_fn = [
    addu(V0, ZERO, ZERO),
    addu(V0, V0, A0),           # loop:
    addiu(A0, A0, -1),
    bne(A0, ZERO, -3),          # back to loop
    NOP,
    jr(RA),
    NOP,
]

data = struct.pack("<8f", 1, 2, 3, 4, 5, 6, 7, 8)


def f32(*xs: float) -> int:
    """Pack up to four floats as a 128-bit register value (x in the low bits)."""
    words = struct.unpack("<4I", struct.pack("<4f", *(list(xs) + [0.0] * (4 - len(xs)))))
    return sum(w << (32 * n) for n, w in enumerate(words))


EXPECTED = {                    # low 64 bits
    S0: 55,
    S1: 0xFFFFFFFFFFFFFFFE,
    S2: 0x8000000000000000,
    S3: 0xFFFFFFFFFFFFFFFD,
    S4: 0xFFFFFFFFFFFFFFFF,
    S5: 0xFFFFFFFFFFFFFFFF,
    S6: 1,
    S7: 3,
    T7: 0x1234,
}
EXPECTED128 = {                 # full 128 bits
    A2: 0x00000000_00000000_11111111_22222222,
    A3: 0x00000000_22222222_00000000_11111111,
    V1: 0x11111111_00000000_00000000_11111111,
    T3: f32(6, 8, 10, 12),
    T4: f32(5, 10, 15, 20),
    T5: f32(-4, 8, -4, 0),
    T6: f32(4 / 5),
}
NAMES = {3: "v1", 6: "a2", 7: "a3", 11: "t3", 12: "t4", 13: "t5", 14: "t6", 15: "t7",
         **{16 + n: f"s{n}" for n in range(8)}}


def build() -> bytes:
    import rabbitizer as rz
    rz.config.pseudos_enablePseudos = False

    code = bytearray(DATA - BASE + len(data))
    for start, instrs in ((BASE, main), (SUM, sum_fn)):
        assert start + len(instrs) * 4 <= (SUM if start == BASE else DATA), "test code overlaps"
        for n, (word, mnemonic) in enumerate(instrs):
            got = rz.Instruction(word, vram=start + n * 4, category=rz.InstrCategory.R5900).getOpcodeName()
            assert got == mnemonic, f"bad encoding at +{n * 4:#x}: {word:08X} decodes as {got}, wanted {mnemonic}"
            struct.pack_into("<I", code, start - BASE + n * 4, word)
    code[DATA - BASE:] = data

    off = 0x100
    ehdr = struct.pack("<4sBBBBB7sHHIIIIIHHHHHH",
                       b"\x7fELF", 1, 1, 1, 0, 0, b"\0" * 7,
                       2, 8, 1, BASE, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    phdr = struct.pack("<IIIIIIII", 1, off, BASE, BASE, len(code), len(code), 7, 0x10)
    return (ehdr + phdr).ljust(off, b"\0") + bytes(code)


if __name__ == "__main__":
    with open(sys.argv[1], "wb") as f:
        f.write(build())
