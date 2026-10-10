"""Translate R5900 functions into C that runs against runtime/ps2_runtime.h."""
import re
from collections import Counter

import rabbitizer as rz

from .functions import Function

I = rz.InstrId


def cname(addr: int) -> str:
    return f"f_{addr:08X}"


def label(addr: int) -> str:
    return f"L_{addr:08X}"


# --- operand helpers -------------------------------------------------------------------------

# Fields come straight from the encoding; rabbitizer refuses to return fields an op doesn't use.
def rs(i): return (i.getRaw() >> 21) & 31
def rt(i): return (i.getRaw() >> 16) & 31
def rd(i): return (i.getRaw() >> 11) & 31
def sa_(i): return (i.getRaw() >> 6) & 31
def ft(i): return (i.getRaw() >> 16) & 31
def fs(i): return (i.getRaw() >> 11) & 31
def fd(i): return (i.getRaw() >> 6) & 31
def imm16(i): return i.getRaw() & 0xFFFF
def simm16(i): v = imm16(i); return v - 0x10000 if v & 0x8000 else v


def U64(n): return f"ctx->r[{n}].ud[0]"
def S64(n): return f"ctx->r[{n}].sd[0]"
def U32(n): return f"ctx->r[{n}].uw[0]"
def S32(n): return f"ctx->r[{n}].sw[0]"
def F(n): return f"ctx->f[{n}].f"
def FU(n): return f"ctx->f[{n}].u"


def set64(n: int, expr: str) -> str:
    """Write the low 64 bits of GPR n. Writes to $zero are discarded (but still evaluated)."""
    if n == 0:
        return f"(void)({expr});"
    return f"{U64(n)} = (uint64_t)({expr});"


def set32s(n: int, expr: str) -> str:
    """Write a 32-bit result, sign-extended to 64 bits (MIPS III rule for 32-bit ops)."""
    return set64(n, f"(int64_t)(int32_t)(uint32_t)({expr})")


def ea(i) -> str:
    return f"(uint32_t)({U32(rs(i))} + {simm16(i)})"


# --- VU0 macro mode (COP2) -----------------------------------------------------------------
# Vector ops encode: dest mask (x=8,y=4,z=2,w=1) in bits 24..21, ft 20..16, fs 15..11, fd 10..6.

LANES = "xyzw"
VU_ARITH = re.compile(r"^v(madd|msub|mini|max|add|sub|mul)(a?)([xyzwqi]?)$")
VU_EXPR = {"add": "{a} + {b}", "sub": "{a} - {b}", "mul": "{a} * {b}",
           "madd": "{acc} + {a} * {b}", "msub": "{acc} - {a} * {b}",
           "max": "fmaxf({a}, {b})", "mini": "fminf({a}, {b})"}


def vdest(i) -> list[int]:
    m = (i.getRaw() >> 21) & 15
    return [k for k in range(4) if m & (8 >> k)]


def VF(n: int, k: int) -> str:
    return f"ctx->vf[{n}].f[{k}]"


def vu_assign(target: str, lanes: list[int], exprs: dict[int, str]) -> str:
    """Compute every lane before writing any, since the destination may also be a source."""
    if not lanes or target == "ctx->vf[0]":
        return "/* writes vf0: no effect */"
    tmp = " ".join(f"float t{k} = {exprs[k]};" for k in lanes)
    st = " ".join(f"{target}.f[{k}] = t{k};" for k in lanes)
    return f"{{ {tmp} {st} }}"


def emit_vu0(i, op: str) -> str | None:
    raw = i.getRaw()
    t, s, d = ft(i), fs(i), fd(i)
    m = VU_ARITH.match(op)
    if m:
        base, to_acc, src = m.groups()
        def b(k):
            if src in ("", None): return VF(t, k)
            if src == "q": return "ctx->vq"
            if src == "i": return "ctx->vi_reg"
            return VF(t, LANES.index(src))
        exprs = {}
        for k in range(4):
            e = VU_EXPR[base].format(a=VF(s, k), b=b(k), acc=f"ctx->vacc.f[{k}]")
            exprs[k] = e if base in ("max", "mini") else f"ps2_fclamp({e})"
        target = "ctx->vacc" if to_acc else f"ctx->vf[{d}]"
        return vu_assign(target, vdest(i), exprs)

    rot1, rot2 = (1, 2, 0), (2, 0, 1)                 # .yzx and .zxy
    if op == "vopmula":
        exprs = {k: f"ps2_fclamp({VF(s, rot1[k])} * {VF(t, rot2[k])})" for k in range(3)}
        return vu_assign("ctx->vacc", [0, 1, 2], exprs)
    if op == "vopmsub":
        exprs = {k: f"ps2_fclamp(ctx->vacc.f[{k}] - {VF(s, rot1[k])} * {VF(t, rot2[k])})" for k in range(3)}
        return vu_assign(f"ctx->vf[{d}]", [0, 1, 2], exprs)

    fsf, ftf = (raw >> 21) & 3, (raw >> 23) & 3        # lane selectors for DIV/SQRT/RSQRT
    if op == "vdiv": return f"ctx->vq = ps2_fdiv({VF(s, fsf)}, {VF(t, ftf)});"
    if op == "vsqrt": return f"ctx->vq = sqrtf(fabsf({VF(t, ftf)}));"
    if op == "vrsqrt": return f"ctx->vq = ps2_fdiv({VF(s, fsf)}, sqrtf(fabsf({VF(t, ftf)})));"
    if op in ("vwaitq", "vnop"): return f"/* {op} */"
    if op == "vmove": return vu_assign(f"ctx->vf[{t}]", vdest(i), {k: VF(s, k) for k in range(4)})
    if op == "vmr32": return vu_assign(f"ctx->vf[{t}]", vdest(i), {k: VF(s, (k + 1) % 4) for k in range(4)})
    if op == "vabs": return vu_assign(f"ctx->vf[{t}]", vdest(i), {k: f"fabsf({VF(s, k)})" for k in range(4)})
    m = re.match(r"^v(ftoi|itof)(0|4|12|15)$", op)
    if m:
        kind, bits = m.group(1), m.group(2)
        lanes = vdest(i)
        if not lanes or t == 0:
            return "/* writes vf0: no effect */"
        if kind == "ftoi":
            body = " ".join(f"u{k} = vu0_ftoi({VF(s, k)}, {bits});" for k in lanes)
            decl = "uint32_t " + ", ".join(f"u{k}" for k in lanes) + ";"
            st = " ".join(f"ctx->vf[{t}].u[{k}] = u{k};" for k in lanes)
            return f"{{ {decl} {body} {st} }}"
        exprs = {k: f"vu0_itof(ctx->vf[{s}].u[{k}], {bits})" for k in range(4)}
        return vu_assign(f"ctx->vf[{t}]", lanes, exprs)

    # transfers between the EE and VU0
    if op == "lqc2": return f"vu0_lqc2(ctx, {rt(i)}, {ea(i)});"
    if op == "sqc2": return f"vu0_sqc2(ctx, {rt(i)}, {ea(i)});"
    if op.startswith("qmtc2"):
        return f"memcpy(&ctx->vf[{rd(i)}], &ctx->r[{rt(i)}], 16);" if rd(i) else "/* qmtc2 to vf0 */"
    if op.startswith("qmfc2"):
        return f"memcpy(&ctx->r[{rt(i)}], &ctx->vf[{rd(i)}], 16);" if rt(i) else "/* qmfc2 to $zero */"
    if op.startswith("cfc2"): return set32s(rt(i), f"vu0_cfc2(ctx, {rd(i)})")
    if op.startswith("ctc2"): return f"vu0_ctc2(ctx, {rd(i)}, {U32(rt(i))});"
    return None


MMI_3OP = {"pextlw", "pextuw", "pextlb", "pextub", "ppacb", "prot3w", "pcpyh", "paddh", "psubw",
           "psubb", "padduw", "pminh", "pmaxh", "pcgth", "qfsrv"}
MMI_SHIFT = {"psllh", "psrlh", "psrah"}


class FunctionTranslator:
    def __init__(self, func: Function, func_starts: set[int], stats: Counter):
        self.f = func
        self.func_starts = func_starts
        self.stats = stats
        self.has_switch = any(i.uniqueId == I.cpu_jr and rs(i) != 31 for i in func.instrs)
        self.labels = self._find_labels()
        # Targets of backward branches are loop heads; tracing them makes spin loops visible.
        self.loop_heads = {i.getBranchVramGeneric() for i in func.instrs
                           if i.isBranch() and i.getBranchVramGeneric() <= i.vram
                           and self.in_func(i.getBranchVramGeneric())}

    def _find_labels(self) -> set[int]:
        if self.has_switch:                       # jump tables can land anywhere
            return {i.vram for i in self.f.instrs}
        out = set()
        for i in self.f.instrs:
            if i.isBranch():
                out.add(i.getBranchVramGeneric())
            elif i.uniqueId == I.cpu_j:
                out.add(i.getInstrIndexAsVram())
        return {a for a in out if self.f.start <= a < self.f.end}

    def in_func(self, addr: int) -> bool:
        return self.f.start <= addr < self.f.end

    # --- control flow ----------------------------------------------------------------------

    def call(self, addr: int) -> str:
        if addr in self.func_starts:
            return f"{cname(addr)}(ctx);"
        return f"ps2_call(ctx, 0x{addr:08X}u);"

    def jump(self, addr: int) -> str:
        """Unconditional transfer: goto inside this function, tail call otherwise."""
        if self.in_func(addr):
            return f"goto {label(addr)};"
        return f"{{ {self.call(addr)} return; }}"

    def branch_cond(self, i) -> str | None:
        u = i.uniqueId
        a, b = S64(rs(i)), S64(rt(i))
        if u in (I.cpu_beq, I.cpu_beql): return f"{a} == {b}"
        if u in (I.cpu_bne, I.cpu_bnel): return f"{a} != {b}"
        if u in (I.cpu_blez, I.cpu_blezl): return f"{a} <= 0"
        if u in (I.cpu_bgtz, I.cpu_bgtzl): return f"{a} > 0"
        if u in (I.cpu_bltz, I.cpu_bltzl, I.cpu_bltzal, I.cpu_bltzall): return f"{a} < 0"
        if u in (I.cpu_bgez, I.cpu_bgezl, I.cpu_bgezal, I.cpu_bgezall): return f"{a} >= 0"
        if u in (I.cpu_bc1t, I.cpu_bc1tl): return "(ctx->fcr31 >> 23) & 1"
        if u in (I.cpu_bc1f, I.cpu_bc1fl): return "!((ctx->fcr31 >> 23) & 1)"
        return None

    def emit_branch(self, i, delay: str) -> list[str]:
        cond = self.branch_cond(i)
        if cond is None:
            self.stats[f"UNIMPL {i.getOpcodeName()}"] += 1
            return [f"PS2_UNIMPL(ctx, 0x{i.vram:08X}u, \"{i.getOpcodeName()}\"); /* branch never taken */",
                    delay]
        target = i.getBranchVramGeneric()
        link = f"{U64(31)} = 0x{i.vram + 8:08X}ull; " if i.doesLink() else ""
        action = self.call(target) if i.doesLink() else self.jump(target)
        if i.isBranchLikely():
            return [f"if ({cond}) {{ {link}{delay} {action} }}"]
        return [f"{{ int br = ({cond}); {link}{delay} if (br) {{ {action} }} }}"]

    def emit_jump(self, i, delay: str) -> list[str]:
        u = i.uniqueId
        if u == I.cpu_j:
            return [delay, self.jump(i.getInstrIndexAsVram())]
        if u == I.cpu_jal:
            return [f"{U64(31)} = 0x{i.vram + 8:08X}ull; {delay}", self.call(i.getInstrIndexAsVram())]
        if u == I.cpu_jalr:
            link = set64(rd(i), f"0x{i.vram + 8:08X}ull")
            return [f"{{ uint32_t t = {U32(rs(i))}; {link} {delay} ps2_call(ctx, t); }}"]
        if u == I.cpu_jr:
            if rs(i) == 31:
                return [f"{delay} return;"]
            cases = " ".join(f"case 0x{a:08X}u: goto {label(a)};" for a in sorted(self.labels))
            return [f"{{ uint32_t t = {U32(rs(i))}; {delay}",
                    f"  switch (t) {{ {cases} default: ps2_call(ctx, t); return; }} }}"]
        raise AssertionError(i.getOpcodeName())

    # --- straight-line instructions --------------------------------------------------------

    def emit(self, i) -> str:
        u, op = i.uniqueId, i.getOpcodeName()
        self.stats[op] += 1
        s, t, d = rs(i), rt(i), rd(i)
        sa = sa_(i)

        if u == I.r5900_ei: return "ctx->ie = 1;"
        if u == I.r5900_di: return "ctx->ie = 0;"
        # no-ops for a single-threaded native build
        if u in (I.cpu_nop, I.cpu_sync, I.r5900_sync_p, I.cpu_cache, I.cpu_pref):
            return f"/* {op} */"
        if i.getRaw() == 0:
            return "/* nop */"

        # 32-bit arithmetic (results sign-extended)
        if u in (I.cpu_addu, I.cpu_add): return set32s(d, f"{U32(s)} + {U32(t)}")
        if u in (I.cpu_subu, I.cpu_sub): return set32s(d, f"{U32(s)} - {U32(t)}")
        if u in (I.cpu_addiu, I.cpu_addi): return set32s(t, f"{U32(s)} + {simm16(i)}")
        if u == I.cpu_lui: return set32s(t, f"0x{imm16(i) << 16:08X}u")
        # 64-bit arithmetic
        if u in (I.cpu_daddu, I.cpu_dadd): return set64(d, f"{U64(s)} + {U64(t)}")
        if u in (I.cpu_dsubu, I.cpu_dsub): return set64(d, f"{U64(s)} - {U64(t)}")
        if u in (I.cpu_daddiu, I.cpu_daddi): return set64(t, f"{U64(s)} + (uint64_t)(int64_t){simm16(i)}")
        # logic (64-bit; immediates are zero-extended)
        if u == I.cpu_and: return set64(d, f"{U64(s)} & {U64(t)}")
        if u == I.cpu_or: return set64(d, f"{U64(s)} | {U64(t)}")
        if u == I.cpu_xor: return set64(d, f"{U64(s)} ^ {U64(t)}")
        if u == I.cpu_nor: return set64(d, f"~({U64(s)} | {U64(t)})")
        if u == I.cpu_andi: return set64(t, f"{U64(s)} & 0x{imm16(i):X}u")
        if u == I.cpu_ori: return set64(t, f"{U64(s)} | 0x{imm16(i):X}u")
        if u == I.cpu_xori: return set64(t, f"{U64(s)} ^ 0x{imm16(i):X}u")
        # comparisons
        if u == I.cpu_slt: return set64(d, f"{S64(s)} < {S64(t)}")
        if u == I.cpu_sltu: return set64(d, f"{U64(s)} < {U64(t)}")
        if u == I.cpu_slti: return set64(t, f"{S64(s)} < {simm16(i)}")
        if u == I.cpu_sltiu: return set64(t, f"{U64(s)} < (uint64_t)(int64_t){simm16(i)}")
        if u == I.cpu_movz: return f"if ({U64(t)} == 0) {{ {set64(d, U64(s))} }}"
        if u == I.cpu_movn: return f"if ({U64(t)} != 0) {{ {set64(d, U64(s))} }}"
        # shifts
        if u == I.cpu_sll: return set32s(d, f"{U32(t)} << {sa}")
        if u == I.cpu_srl: return set32s(d, f"{U32(t)} >> {sa}")
        if u == I.cpu_sra: return set32s(d, f"{S32(t)} >> {sa}")
        if u == I.cpu_sllv: return set32s(d, f"{U32(t)} << ({U32(s)} & 31)")
        if u == I.cpu_srlv: return set32s(d, f"{U32(t)} >> ({U32(s)} & 31)")
        if u == I.cpu_srav: return set32s(d, f"{S32(t)} >> ({U32(s)} & 31)")
        if u == I.cpu_dsll: return set64(d, f"{U64(t)} << {sa}")
        if u == I.cpu_dsrl: return set64(d, f"{U64(t)} >> {sa}")
        if u == I.cpu_dsra: return set64(d, f"{S64(t)} >> {sa}")
        if u == I.cpu_dsll32: return set64(d, f"{U64(t)} << {sa + 32}")
        if u == I.cpu_dsrl32: return set64(d, f"{U64(t)} >> {sa + 32}")
        if u == I.cpu_dsra32: return set64(d, f"{S64(t)} >> {sa + 32}")
        if u == I.cpu_dsllv: return set64(d, f"{U64(t)} << ({U32(s)} & 63)")
        if u == I.cpu_dsrlv: return set64(d, f"{U64(t)} >> ({U32(s)} & 63)")
        if u == I.cpu_dsrav: return set64(d, f"{S64(t)} >> ({U32(s)} & 63)")
        # multiply / divide (pipeline 0 and the R5900's pipeline 1); R5900 mult also writes rd
        for ids, pipe in (((I.cpu_mult, I.r5900_mult, I.cpu_multu), ""), ((I.r5900_mult1, I.r5900_multu1), "1")):
            if u in ids:
                signed = u in (I.cpu_mult, I.r5900_mult, I.r5900_mult1)
                cast = "int64_t)(int32_t" if signed else "uint64_t)(uint32_t"
                return (f"{{ uint64_t p = (uint64_t)(({cast}){U32(s)} * ({cast}){U32(t)}); "
                        f"ctx->lo{pipe} = (uint64_t)(int64_t)(int32_t)p; ctx->hi{pipe} = (uint64_t)(int64_t)(int32_t)(p >> 32); "
                        f"{set64(d, f'ctx->lo{pipe}') if d else ''} }}")
        for ids, pipe in (((I.r5900_madd, I.r5900_maddu), ""), ((I.r5900_madd1, I.r5900_maddu1), "1")):
            if u in ids:
                signed = u in (I.r5900_madd, I.r5900_madd1)
                cast = "int64_t)(int32_t" if signed else "uint64_t)(uint32_t"
                return (f"{{ uint64_t p = (((uint64_t)ctx->hi{pipe} << 32) | (uint32_t)ctx->lo{pipe}) + "
                        f"(uint64_t)(({cast}){U32(s)} * ({cast}){U32(t)}); "
                        f"ctx->lo{pipe} = (uint64_t)(int64_t)(int32_t)p; ctx->hi{pipe} = (uint64_t)(int64_t)(int32_t)(p >> 32); "
                        f"{set64(d, f'ctx->lo{pipe}') if d else ''} }}")
        for ids, pipe in (((I.cpu_div, I.cpu_divu), ""), ((I.r5900_div1, I.r5900_divu1), "1")):
            if u in ids:
                fn = "ps2_div" if u in (I.cpu_div, I.r5900_div1) else "ps2_divu"
                return f"{fn}(&ctx->lo{pipe}, &ctx->hi{pipe}, {U32(s)}, {U32(t)});"
        if u == I.cpu_mfhi: return set64(d, "ctx->hi")
        if u == I.cpu_mflo: return set64(d, "ctx->lo")
        if u == I.r5900_mfhi1: return set64(d, "ctx->hi1")
        if u == I.r5900_mflo1: return set64(d, "ctx->lo1")
        if u == I.cpu_mthi: return f"ctx->hi = {U64(s)};"
        if u == I.cpu_mtlo: return f"ctx->lo = {U64(s)};"
        if u == I.r5900_mthi1: return f"ctx->hi1 = {U64(s)};"
        if u == I.r5900_mtlo1: return f"ctx->lo1 = {U64(s)};"
        if u == I.r5900_mfsa: return set64(d, "ctx->sa")
        if u == I.r5900_mtsa: return f"ctx->sa = {U32(s)};"
        if u == I.r5900_mtsab: return f"ctx->sa = (({U32(s)} ^ 0x{imm16(i):X}u) & 15u) * 8u;"
        if u == I.r5900_mtsah: return f"ctx->sa = (({U32(s)} ^ 0x{imm16(i):X}u) & 7u) * 16u;"
        if op in MMI_3OP: return f"mmi_{op}(ctx, {d}, {s}, {t});"
        if op in MMI_SHIFT: return f"mmi_{op}(ctx, {d}, {t}, {sa});"
        vu = emit_vu0(i, op)
        if vu is not None:
            return vu
        # loads
        if u == I.cpu_lb: return set64(t, f"(int64_t)(int8_t)ps2_r8(ctx, {ea(i)})")
        if u == I.cpu_lbu: return set64(t, f"ps2_r8(ctx, {ea(i)})")
        if u == I.cpu_lh: return set64(t, f"(int64_t)(int16_t)ps2_r16(ctx, {ea(i)})")
        if u == I.cpu_lhu: return set64(t, f"ps2_r16(ctx, {ea(i)})")
        if u == I.cpu_lw: return set64(t, f"(int64_t)(int32_t)ps2_r32(ctx, {ea(i)})")
        if u == I.cpu_lwu: return set64(t, f"ps2_r32(ctx, {ea(i)})")
        if u == I.cpu_ld: return set64(t, f"ps2_r64(ctx, {ea(i)})")
        if u == I.r5900_lq:
            return f"{{ r128 v = ps2_r128(ctx, {ea(i)} & ~15u); {'ctx->r[%d] = v;' % t if t else '(void)v;'} }}"
        if u in (I.cpu_lwl, I.cpu_lwr, I.cpu_ldl, I.cpu_ldr):
            return f"ps2_{op}(ctx, {t}, {ea(i)});" if t else f"/* {op} to $zero */"
        # stores
        if u == I.cpu_sb: return f"ps2_w8(ctx, {ea(i)}, (uint8_t){U32(t)});"
        if u == I.cpu_sh: return f"ps2_w16(ctx, {ea(i)}, (uint16_t){U32(t)});"
        if u == I.cpu_sw: return f"ps2_w32(ctx, {ea(i)}, {U32(t)});"
        if u == I.cpu_sd: return f"ps2_w64(ctx, {ea(i)}, {U64(t)});"
        if u == I.r5900_sq: return f"ps2_w128(ctx, {ea(i)} & ~15u, ctx->r[{t}]);"
        if u in (I.cpu_swl, I.cpu_swr, I.cpu_sdl, I.cpu_sdr):
            return f"ps2_{op}(ctx, {t}, {ea(i)});"
        # 128-bit MMI moves/logic used by compiled code for struct copies
        mmi = {I.r5900_por: "a.ud[{k}] | b.ud[{k}]", I.r5900_pand: "a.ud[{k}] & b.ud[{k}]",
               I.r5900_pxor: "a.ud[{k}] ^ b.ud[{k}]", I.r5900_pnor: "~(a.ud[{k}] | b.ud[{k}])"}
        if u in mmi:
            lo, hi = mmi[u].format(k=0), mmi[u].format(k=1)
            return (f"{{ r128 a = ctx->r[{s}], b = ctx->r[{t}]; "
                    f"{'ctx->r[%d].ud[0] = %s; ctx->r[%d].ud[1] = %s;' % (d, lo, d, hi) if d else ''} }}")
        if u == I.r5900_pcpyld:
            return f"{{ uint64_t a = ctx->r[{s}].ud[0], b = ctx->r[{t}].ud[0]; " + \
                   (f"ctx->r[{d}].ud[0] = b; ctx->r[{d}].ud[1] = a; }}" if d else "(void)a; (void)b; }")
        if u == I.r5900_pcpyud:
            return f"{{ uint64_t a = ctx->r[{s}].ud[1], b = ctx->r[{t}].ud[1]; " + \
                   (f"ctx->r[{d}].ud[0] = a; ctx->r[{d}].ud[1] = b; }}" if d else "(void)a; (void)b; }")
        # FPU (COP1). Note: the PS2 FPU is not IEEE 754; see notes/recompiler-design.md.
        if u == I.cpu_lwc1: return f"{FU(ft(i))} = ps2_r32(ctx, {ea(i)});"
        if u == I.cpu_swc1: return f"ps2_w32(ctx, {ea(i)}, {FU(ft(i))});"
        if u == I.cpu_mtc1: return f"{FU(fs(i))} = {U32(t)};"
        if u == I.cpu_mfc1: return set32s(t, FU(fs(i)))
        if u == I.cpu_ctc1: return f"if ({fs(i)} == 31) ctx->fcr31 = {U32(t)};"
        if u == I.cpu_cfc1: return set32s(t, f"({fs(i)} == 31) ? ctx->fcr31 : 0x2E00u")
        if u == I.cpu_mov_s: return f"{F(fd(i))} = {F(fs(i))};"
        if u == I.cpu_neg_s: return f"{F(fd(i))} = -{F(fs(i))};"
        if u == I.cpu_abs_s: return f"{F(fd(i))} = fabsf({F(fs(i))});"
        if u == I.cpu_add_s: return f"{F(fd(i))} = ps2_fclamp({F(fs(i))} + {F(ft(i))});"
        if u == I.cpu_sub_s: return f"{F(fd(i))} = ps2_fclamp({F(fs(i))} - {F(ft(i))});"
        if u == I.cpu_mul_s: return f"{F(fd(i))} = ps2_fclamp({F(fs(i))} * {F(ft(i))});"
        if u == I.cpu_div_s: return f"{F(fd(i))} = ps2_fdiv({F(fs(i))}, {F(ft(i))});"
        if u in (I.cpu_sqrt_s, I.r5900_c1__sqrt_s): return f"{F(fd(i))} = sqrtf(fabsf({F(ft(i))}));"
        if u == I.r5900_rsqrt_s: return f"{F(fd(i))} = ps2_fdiv({F(fs(i))}, sqrtf(fabsf({F(ft(i))})));"
        if u == I.r5900_max_s: return f"{F(fd(i))} = fmaxf({F(fs(i))}, {F(ft(i))});"
        if u == I.r5900_min_s: return f"{F(fd(i))} = fminf({F(fs(i))}, {F(ft(i))});"
        if u == I.r5900_adda_s: return f"ctx->acc = ps2_fclamp({F(fs(i))} + {F(ft(i))});"
        if u == I.r5900_suba_s: return f"ctx->acc = ps2_fclamp({F(fs(i))} - {F(ft(i))});"
        if u == I.r5900_mula_s: return f"ctx->acc = ps2_fclamp({F(fs(i))} * {F(ft(i))});"
        if u == I.r5900_madda_s: return f"ctx->acc = ps2_fclamp(ctx->acc + {F(fs(i))} * {F(ft(i))});"
        if u == I.r5900_msuba_s: return f"ctx->acc = ps2_fclamp(ctx->acc - {F(fs(i))} * {F(ft(i))});"
        if u == I.r5900_madd_s: return f"{F(fd(i))} = ps2_fclamp(ctx->acc + {F(fs(i))} * {F(ft(i))});"
        if u == I.r5900_msub_s: return f"{F(fd(i))} = ps2_fclamp(ctx->acc - {F(fs(i))} * {F(ft(i))});"
        if u == I.cpu_cvt_s_w: return f"{F(fd(i))} = (float)(int32_t){FU(fs(i))};"
        if u in (I.cpu_cvt_w_s, I.cpu_trunc_w_s): return f"{FU(fd(i))} = (uint32_t)ps2_f2i({F(fs(i))});"
        if u == I.cpu_c_eq_s: return f"ps2_fcond(ctx, {F(fs(i))} == {F(ft(i))});"
        if u in (I.cpu_c_lt_s, I.r5900_c_lt_s): return f"ps2_fcond(ctx, {F(fs(i))} < {F(ft(i))});"
        if u in (I.cpu_c_le_s, I.r5900_c_le_s): return f"ps2_fcond(ctx, {F(fs(i))} <= {F(ft(i))});"
        if u == I.cpu_c_f_s: return "ps2_fcond(ctx, 0);"
        # system
        if u == I.cpu_syscall: return f"ps2_syscall(ctx, 0x{i.vram:08X}u);"
        if u == I.cpu_mfc0: return set32s(t, f"ps2_mfc0(ctx, {d})")
        if u == I.cpu_mtc0: return f"ps2_mtc0(ctx, {d}, {U32(t)});"
        if u in (I.cpu_break, I.cpu_teq, I.cpu_tne, I.cpu_tge, I.cpu_tgeu, I.cpu_tlt, I.cpu_tltu):
            return f"/* {op}: traps ignored */"

        self.stats[f"UNIMPL {op}"] += 1
        return f"PS2_UNIMPL(ctx, 0x{i.vram:08X}u, \"{op}\");"

    # --- whole function --------------------------------------------------------------------

    def translate(self) -> str:
        f = self.f
        out = [f"/* {f.name}: 0x{f.start:08X}-0x{f.end:08X} */", f"void {cname(f.start)}(ps2_ctx* ctx) {{",
               f"    PS2_TRACE(0x{f.start:08X}u);"]
        instrs = f.instrs
        k = 0
        while k < len(instrs):
            i = instrs[k]
            asm = i.disassemble().replace("*/", "* /")
            if i.vram in self.labels:
                out.append(f"{label(i.vram)}:;")
                if i.vram in self.loop_heads:
                    out.append(f"    PS2_TRACE(0x{i.vram:08X}u); PS2_POLL_IRQ(ctx);")
            if i.isBranch() or i.isJump():
                if k + 1 < len(instrs):
                    slot = instrs[k + 1]
                    delay = self.emit(slot)
                    self.stats[i.getOpcodeName()] += 1
                    lines = (self.emit_branch(i, delay) if i.isBranch() else self.emit_jump(i, delay))
                    out.append(f"    /* {i.vram:08X}: {asm} */")
                    out += [f"    {ln}" for ln in lines]
                    if slot.vram in self.labels:          # something jumps straight to the delay slot
                        out.append(f"    if (0) {{ {label(slot.vram)}:; {delay} }}")
                    k += 2
                    continue
                self.stats["branch at end of function"] += 1
            out.append(f"    /* {i.vram:08X}: {asm} */")
            out.append(f"    {self.emit(i)}")
            k += 1
        out.append(f"    {self.call(f.end)} /* fell off the end */")
        out.append("}")
        return "\n".join(out)
