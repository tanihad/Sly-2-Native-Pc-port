"""Generate the IOP recompiler test: two synthetic IRX modules (hand-encoded MIPS I) are compiled
by `recomp/iop.py` exactly like the game's modules, and test cases with independently computed
expectations are written for test_iop_recomp.c.

    python gen.py <output dir>
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(HERE))

from irx_asm import *  # noqa: E402,F403
from recomp import irx, iop  # noqa: E402

M = 0xFFFFFFFF
LIB_BASE, MOD_BASE = 0x00040000, 0x00060000


def s32(v): v &= M; return v - (1 << 32) if v & 0x80000000 else v


# ---- module "test_lib": exports library "testlib" ---------------------------------------------
lib = Asm()
lib.label("start"); lib.jr(RA); lib.addiu(V0, ZERO, 0)
lib.label("lib_nop"); lib.jr(RA); lib.nop()
lib.label("add3"); lib.addu(V0, A0, A1); lib.jr(RA); lib.addu(V0, V0, A2)
lib.export_table("testlib", ["lib_nop", "lib_nop", "lib_nop", "lib_nop", "add3"])

# ---- module "test_mod" -----------------------------------------------------------------------------
a = Asm()
cases = []          # (name, function label, {reg: value}, {reg: expected}, {data label+off: expected word})

# _start(argc, argv): records argc and the first character of argv[1]; resident (0)
a.label("start")
a.la(T0, "start_info")
a.sw(A0, 0, T0)
a.lw(T1, 4, A1)
a.nop()
a.lbu(T1, 0, T1)
a.nop()
a.sw(T1, 4, T0)
a.jr(RA)
a.addu(V0, ZERO, ZERO)

# ALU
a.label("f_alu")
a.addu(V0, A0, A1); a.subu(V1, A0, A1)
a.and_(T0, A0, A1); a.or_(T1, A0, A1); a.xor(T2, A0, A1); a.nor(T3, A0, A1)
a.andi(T4, A0, 0x8001); a.ori(T5, A0, 0x8001); a.xori(T6, A0, 0xFFFF)
a.slt(T7, A0, A1); a.sltu(T8, A0, A1); a.slti(T9, A0, -1); a.sltiu(S0, A0, -1)
a.addiu(S1, A1, 1); a.lui(S2, 0x8000); a.ori(S2, S2, 0x1234)
a.slt(S3, A1, A0); a.sltu(S4, A1, A0); a.sltiu(S5, A1, 0x8000)
a.jr(RA); a.nop()
x, y = 0x80000001, 0x7FFFFFFF
cases.append(("alu", "f_alu", {A0: x, A1: y}, {
    V0: (x + y) & M, V1: (x - y) & M, T0: x & y, T1: x | y, T2: x ^ y, T3: ~(x | y) & M,
    T4: x & 0x8001, T5: x | 0x8001, T6: x ^ 0xFFFF, T7: int(s32(x) < s32(y)), T8: int(x < y),
    T9: int(s32(x) < -1), S0: int(x < 0xFFFFFFFF), S1: (y + 1) & M, S2: 0x80001234,
    S3: int(s32(y) < s32(x)), S4: int(y < x), S5: int(y < 0xFFFF8000)}, {}))

# shifts (variable shifts use the low 5 bits of rs)
a.label("f_shift")
a.sll(V0, A0, 4); a.srl(V1, A0, 4); a.sra(T0, A0, 4)
a.sllv(T1, A0, A1); a.srlv(T2, A0, A1); a.srav(T3, A0, A1)
a.sra(T4, A0, 31); a.srl(T5, A0, 31); a.sll(T6, A0, 0)
a.jr(RA); a.nop()
x, n = 0x80000010, 36
cases.append(("shift", "f_shift", {A0: x, A1: n}, {
    V0: (x << 4) & M, V1: x >> 4, T0: (s32(x) >> 4) & M, T1: (x << (n & 31)) & M, T2: x >> (n & 31),
    T3: (s32(x) >> (n & 31)) & M, T4: M, T5: 1, T6: x}, {}))

# multiply / divide, including the R3000A's divide-by-zero and overflow results
a.label("f_muldiv")
a.mult(A0, A1); a.mflo(V0); a.mfhi(V1)
a.multu(A0, A1); a.mflo(T0); a.mfhi(T1)
a.div(A0, A1); a.mflo(T2); a.mfhi(T3)
a.divu(A0, A1); a.mflo(T4); a.mfhi(T5)
a.div(A0, ZERO); a.mflo(T6); a.mfhi(T7)
a.lui(T8, 0x8000); a.addiu(T9, ZERO, -1)
a.div(T8, T9); a.mflo(S0); a.mfhi(S1)
a.divu(A1, ZERO); a.mflo(S2); a.mfhi(S3)
a.addiu(S4, ZERO, 7); a.div(S4, ZERO); a.mflo(S4)
a.mthi(A1); a.mtlo(A0); a.mfhi(S5); a.mflo(S6)
a.jr(RA); a.nop()
x, y = 0xFFFFFFF9, 2          # -7, 2
p, pu = s32(x) * y, x * y
cases.append(("muldiv", "f_muldiv", {A0: x, A1: y}, {
    V0: p & M, V1: (p >> 32) & M, T0: pu & M, T1: (pu >> 32) & M,
    T2: (-3) & M, T3: (-1) & M,                     # C-style truncation toward zero
    T4: x // y, T5: x % y,
    T6: 1, T7: x,                                   # div by 0: lo = 1 for a negative dividend, hi = dividend
    S0: 0x80000000, S1: 0,                          # INT_MIN / -1
    S2: M, S3: y,                                   # divu by 0
    S4: M,                                          # positive / 0: lo = -1
    S5: y, S6: x}, {}))

# loads and stores, sign/zero extension, unaligned lwl/lwr/swl/swr at every alignment
a.dlabel("membuf", 16)
a.dbytes(bytes(range(0x80, 0xA0)))
a.label("f_mem")
a.la(A0, "membuf")
a.lb(V0, 1, A0); a.lbu(V1, 1, A0); a.lh(T0, 2, A0); a.lhu(T1, 2, A0); a.lw(T2, 4, A0)
a.lwr(T3, 5, A0); a.lwl(T3, 8, A0)                 # unaligned word at 5
for k, reg in enumerate((T4, T5, T6, T7)):         # lwl alone at offsets 8..11 into 0x11223344
    a.lui(reg, 0x1122); a.ori(reg, reg, 0x3344); a.lwl(reg, 8 + k, A0)
for k, reg in enumerate((S0, S1, S2, S3)):         # lwr alone at offsets 12..15
    a.lui(reg, 0x1122); a.ori(reg, reg, 0x3344); a.lwr(reg, 12 + k, A0)
a.lui(T8, 0xCAFE); a.ori(T8, T8, 0xBABE)
a.sw(T8, 16, A0); a.sh(T8, 20, A0); a.sb(T8, 23, A0)
a.swl(T8, 24, A0); a.swr(T8, 29, A0)               # partial stores into 0x98..0x9F
a.jr(RA); a.nop()
buf = bytearray(range(0x80, 0xA0))
def word(off): return struct.unpack_from("<I", buf, off)[0]
def lwl(rtv, addr): k = addr & 3; w = word(addr & ~3); s = 8 * (3 - k); return ((w << s) | (rtv & ((1 << s) - 1))) & M
def lwr(rtv, addr): k = addr & 3; w = word(addr & ~3); s = 8 * k; return ((w >> s) | (rtv & ~(M >> s))) & M
exp = {V0: 0xFFFFFF81, V1: 0x81, T0: 0xFFFF8382, T1: 0x8382, T2: word(4), T3: lwl(lwr(0, 5), 8)}
for k, reg in enumerate((T4, T5, T6, T7)): exp[reg] = lwl(0x11223344, 8 + k)
for k, reg in enumerate((S0, S1, S2, S3)): exp[reg] = lwr(0x11223344, 12 + k)
v = 0xCAFEBABE
mem = bytearray(buf)
struct.pack_into("<I", mem, 16, v); struct.pack_into("<H", mem, 20, v & 0xFFFF); mem[23] = v & 0xFF
w = struct.unpack_from("<I", mem, 24)[0]; s = 8 * (3 - 0); w = (w & ~(M >> s)) | (v >> s); struct.pack_into("<I", mem, 24, w & M)
w = struct.unpack_from("<I", mem, 28)[0]; s = 8 * 1; w = (w & ((1 << s) - 1)) | (v << s); struct.pack_into("<I", mem, 28, w & M)
cases.append(("mem", "f_mem", {}, exp, {("membuf", o): struct.unpack_from("<I", mem, o)[0] for o in (16, 20, 24, 28)}))

# branches and delay slots; link registers of bltzal/bgezal; a counted loop
a.label("f_branch")
a.addu(T8, RA, ZERO)
a.addiu(V0, ZERO, 0)
a.beq(A0, A1, "b1"); a.addiu(V0, V0, 1)            # not taken, delay slot runs
a.addiu(V0, V0, 0x10)
a.label("b1")
a.bne(A0, A1, "b2"); a.addiu(V0, V0, 0x100)        # taken, delay slot runs
a.addiu(V0, V0, 0x1000)
a.label("b2")
a.blez(A0, "b3"); a.addiu(V1, ZERO, 1)             # a0 = -5: taken
a.addiu(V1, V1, 2)
a.label("b3")
a.bgtz(A0, "b4"); a.addiu(V1, V1, 4)               # not taken
a.addiu(V1, V1, 8)
a.label("b4")
a.bltz(A1, "b5"); a.addiu(T0, ZERO, 1)             # a1 = 3: not taken
a.addiu(T0, T0, 2)
a.label("b5")
a.bgez(A1, "b6"); a.addiu(T0, T0, 4)               # taken
a.addiu(T0, T0, 8)
a.label("b6")
a.bltzal(A1, "sub_set_t1"); a.nop()                # not taken, but $ra is written
a.label("after_bltzal")
a.addu(T2, RA, ZERO)
a.addiu(T1, ZERO, 0)
a.bgezal(A1, "sub_set_t1"); a.addiu(T3, ZERO, 7)   # taken: a call
a.label("after_bgezal")
a.addu(T4, RA, ZERO)
a.addiu(T5, ZERO, 0); a.addiu(T6, ZERO, 10)        # t5 = 10 + 9 + ... + 1
a.label("loop")
a.addu(T5, T5, T6)
a.addiu(T6, T6, -1)
a.bne(T6, ZERO, "loop"); a.nop()
a.addu(RA, T8, ZERO)
a.jr(RA); a.nop()
a.label("sub_set_t1")
a.jr(RA); a.addiu(T1, ZERO, 0x55)
xb = MOD_BASE
cases.append(("branch", "f_branch", {A0: (-5) & M, A1: 3}, {
    V0: 0x111, V1: 1 + 4 + 8, T0: 1 + 2 + 4, T1: 0x55, T3: 7, T5: 55,
    T2: None, T4: None}, {}))      # link values filled in below (need the final layout)

# load delay slot: the instruction right after a load sees the register's old value
a.dlabel("hazard_word")
a.dword(0x1234)
a.label("f_hazard")
a.la(A0, "hazard_word")
a.lw(T0, 0, A0)
a.addu(V0, T0, ZERO)                               # old t0
a.addu(V1, T0, ZERO)                               # loaded value
a.jr(RA); a.nop()
cases.append(("load delay", "f_hazard", {T0: 0x77}, {V0: 0x77, V1: 0x1234}, {}))

# switch through a relocated jump table in .data
a.label("f_switch")
a.sltiu(V0, A0, 4)
a.beq(V0, ZERO, "sw_default"); a.nop()
a.la(V1, "jtab")
a.sll(T0, A0, 2)
a.addu(T0, T0, V1)
a.lw(T1, 0, T0)
a.nop()
a.jr(T1); a.nop()
for k in range(4):
    a.label(f"sw_case{k}")
    a.jr(RA); a.addiu(V0, ZERO, 10 * (k + 1))
a.label("sw_default")
a.jr(RA); a.addiu(V0, ZERO, 99)
a.dlabel("jtab")
for k in range(4):
    a.dptr(f"sw_case{k}")
for k, want in ((0, 10), (2, 30), (3, 40), (7, 99)):
    cases.append((f"switch {k}", "f_switch", {A0: k}, {V0: want}, {}))

# indirect call through a pointer built with lui/addiu, and a tail call with j
a.label("f_target")
a.jr(RA); a.addu(V0, A0, A0)
a.label("f_indirect")
a.addiu(SP, SP, -24); a.sw(RA, 20, SP)
a.la(T9, "f_target")
a.jalr(T9); a.addiu(A0, ZERO, 21)
a.addiu(V0, V0, 1)
a.lw(RA, 20, SP); a.nop()
a.jr(RA); a.addiu(SP, SP, 24)
cases.append(("jalr", "f_indirect", {SP: 0x1F0000}, {V0: 43, SP: 0x1F0000}, {}))
a.label("f_tail")
a.addiu(A0, A0, 1)
a.j("f_target"); a.nop()
cases.append(("j tail call", "f_tail", {A0: 4}, {V0: 10}, {}))

# writes to $zero are discarded
a.label("f_zero")
a.addiu(ZERO, ZERO, 5); a.lui(ZERO, 0x1234); a.addu(V0, ZERO, ZERO)
a.jr(RA); a.nop()
cases.append(("zero register", "f_zero", {}, {V0: 0}, {}))

# imports: a native library (sysclib memset), another recompiled module (testlib add3), and a
# 5-argument native call with its 5th argument on the stack (libsd sceSdVoiceTrans)
a.dlabel("fillbuf", 16)
a.dbytes(bytes(16))
a.label("f_imports")
a.addiu(SP, SP, -32); a.sw(RA, 28, SP); a.sw(S0, 24, SP)
a.la(A0, "fillbuf"); a.addiu(A1, ZERO, 0x5A)
a.jal("__imp_sysclib_14"); a.addiu(A2, ZERO, 6)
a.addiu(A0, ZERO, 1); a.addiu(A1, ZERO, 2)
a.jal("__imp_testlib_4"); a.addiu(A2, ZERO, 3)
a.addu(S0, V0, ZERO)
a.addiu(T0, ZERO, 0x800); a.sw(T0, 16, SP)
a.addiu(A0, ZERO, 1); a.addiu(A1, ZERO, 0); a.lui(A2, 0x0012); a.ori(A2, A2, 0x3456)
a.jal("__imp_libsd_17"); a.ori(A3, ZERO, 0x5000)
a.addu(V1, V0, ZERO)
a.addu(V0, S0, ZERO)
a.lw(S0, 24, SP); a.lw(RA, 28, SP); a.nop()
a.jr(RA); a.addiu(SP, SP, 32)
cases.append(("imports", "f_imports", {SP: 0x1F0000, S0: 0xABCD}, {V0: 6, V1: 0x800, S0: 0xABCD, SP: 0x1F0000},
              {("fillbuf", 0): 0x5A5A5A5A, ("fillbuf", 4): 0x00005A5A}))

a.dlabel("start_info")
a.dword(0); a.dword(0)

# import tables last: the recompiler treats everything after them as data
stubs = {}
stubs.update(a.import_table("sysclib", [14]))
stubs.update(a.import_table("testlib", [4]))
stubs.update(a.import_table("libsd", [17]))


def c_array(name: str, data: bytes) -> str:
    rows = [", ".join(f"0x{b:02X}" for b in data[k:k + 16]) for k in range(0, len(data), 16)]
    return f"const unsigned char {name}[{len(data)}] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def main() -> int:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    lib_irx = build_irx("test_lib", lib, "start")
    mod_irx = build_irx("test_mod", a, "start", bss=0x40)
    pairs = iop.prepare([(irx.parse(lib_irx), LIB_BASE), (irx.parse(mod_irx), MOD_BASE)])
    report = iop.write_c(pairs, str(out / "gen"))
    print(report)
    if "(0 untranslated instructions, 1 load-delay hazards)" not in report or "warning" in report:
        print("unexpected generator report")
        return 1
    starts = {s for s, _ in pairs[1][1].functions}
    for fn in ("start", "f_alu", "f_branch", "sub_set_t1", "f_switch", "f_target", "f_indirect", "f_tail"):
        if MOD_BASE + a.addr(fn) not in starts:
            print(f"function {fn} was not discovered")
            return 1
    for k in range(4):                                  # switch cases are labels, not functions
        if MOD_BASE + a.addr(f"sw_case{k}") in starts:
            print(f"switch case {k} was split into a function")
            return 1

    # link-register expectations need the final layout
    for c in cases:
        if c[0] == "branch":
            c[3][T2] = MOD_BASE + a.addr("after_bltzal")
            c[3][T4] = MOD_BASE + a.addr("after_bgezal")

    lines = ['#include <stdint.h>', '',
             'typedef struct { const char* name; uint32_t fn; int nin; uint32_t in[8][2];',
             '                 int nout; uint32_t out[24][2]; int nmem; uint32_t mem[4][2]; } test_case;',
             f"#define LIB_BASE 0x{LIB_BASE:08X}u", f"#define MOD_BASE 0x{MOD_BASE:08X}u",
             f"#define START_INFO 0x{MOD_BASE + a.addr('start_info'):08X}u",
             "static const test_case cases[] = {"]
    for name, fn, inp, outp, memp in cases:
        ins = ", ".join(f"{{{r}, 0x{v & M:08X}u}}" for r, v in inp.items()) or "{0, 0}"
        outs = ", ".join(f"{{{r}, 0x{v & M:08X}u}}" for r, v in outp.items())
        mems = ", ".join(f"{{0x{MOD_BASE + a.addr(lbl) + off:08X}u, 0x{v:08X}u}}" for (lbl, off), v in memp.items()) or "{0, 0}"
        lines.append(f'    {{ "{name}", 0x{MOD_BASE + a.addr(fn):08X}u, {len(inp)}, {{ {ins} }}, {len(outp)}, {{ {outs} }}, '
                     f"{len(memp)}, {{ {mems} }} }},")
    lines.append("};")
    lines.append(c_array("test_lib_irx", lib_irx))
    lines.append(c_array("test_mod_irx", mod_irx))
    (out / "cases.h").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
