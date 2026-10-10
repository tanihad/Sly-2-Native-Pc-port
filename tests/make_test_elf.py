"""Build a tiny PS2-style MIPS ELF from hand-encoded instructions, for testing the recompiler.

The program leaves known results in $s0-$s7 and returns from its entry point. EXPECTED lists
the correct value of each register, checked against a real R5900's semantics.
"""
import struct
import sys

BASE = 0x00100000

ZERO, V0, A0, A1, T0, T1, T2, S0, S1, S2, S3, S4, S5, S6, S7, SP, RA = \
    0, 2, 4, 5, 8, 9, 10, 16, 17, 18, 19, 20, 21, 22, 23, 29, 31


def r(funct, rs=0, rt=0, rd=0, sa=0): return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct
def i(op, rs, rt, imm): return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
def j(op, target): return (op << 26) | ((target >> 2) & 0x3FFFFFF)

def addiu(rt, rs, imm): return i(0x09, rs, rt, imm)
def lui(rt, imm): return i(0x0F, 0, rt, imm)
def ori(rt, rs, imm): return i(0x0D, rs, rt, imm)
def lw(rt, off, base): return i(0x23, base, rt, off)
def sw(rt, off, base): return i(0x2B, base, rt, off)
def bne(rs, rt, off): return i(0x05, rs, rt, off)
def beql(rs, rt, off): return i(0x14, rs, rt, off)
def jal(target): return j(0x03, target)
def addu(rd, rs, rt): return r(0x21, rs, rt, rd)
def sll(rd, rt, sa): return r(0x00, 0, rt, rd, sa)
def dsll32(rd, rt, sa): return r(0x3C, 0, rt, rd, sa)
def div(rs, rt): return r(0x1A, rs, rt)
def mult(rs, rt): return r(0x18, rs, rt)
def mflo(rd): return r(0x12, rd=rd)
def mfhi(rd): return r(0x10, rd=rd)
def jr(rs): return r(0x08, rs)
NOP = 0

SUM = BASE + 0x100          # sum(n) = n + (n-1) + ... + 1

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

EXPECTED = {
    S0: 55,
    S1: 0xFFFFFFFFFFFFFFFE,
    S2: 0x8000000000000000,
    S3: 0xFFFFFFFFFFFFFFFD,
    S4: 0xFFFFFFFFFFFFFFFF,
    S5: 0xFFFFFFFFFFFFFFFF,
    S6: 1,
    S7: 3,
}


def build() -> bytes:
    code = bytearray(0x100 + len(sum_fn) * 4)
    for n, w in enumerate(main):
        struct.pack_into("<I", code, n * 4, w)
    assert len(main) * 4 <= 0x100
    for n, w in enumerate(sum_fn):
        struct.pack_into("<I", code, 0x100 + n * 4, w)

    off = 0x100
    ehdr = struct.pack("<4sBBBBB7sHHIIIIIHHHHHH",
                       b"\x7fELF", 1, 1, 1, 0, 0, b"\0" * 7,
                       2, 8, 1, BASE, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    phdr = struct.pack("<IIIIIIII", 1, off, BASE, BASE, len(code), len(code), 7, 0x10)
    return (ehdr + phdr).ljust(off, b"\0") + bytes(code)


if __name__ == "__main__":
    with open(sys.argv[1], "wb") as f:
        f.write(build())
