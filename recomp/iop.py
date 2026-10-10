"""IOP mode: statically recompile IOP modules (IRX files, MIPS I / R3000A) to C.

    python -m recomp iop <game elf> -o build/sly2/iop

The sound driver modules embedded in the game executable (989snd_Library and Sucker Punch's spiop)
are relocated to fixed IOP addresses (MODULES below; memory map in runtime/iop.h) and every
function is translated to C that runs against runtime/iop.h: 32 general registers of 32 bits,
memory accesses go to the 2 MB IOP RAM, no 64-bit operations exist on the R3000A.

Imports (`jr $ra; addiu $zero, $zero, N` stubs) become direct calls: into the module that exports
the library when both are recompiled (spiop -> snd989), otherwise into the native C library
`iop_imp_<lib>_<function>` (runtime/iop_*.c). The runtime loads the module image the EE sends
through LOADFILE, relocates it to the same address and checks it against the CRCs recorded here.

Load delay slots: the R3000A does not interlock loads, so the instruction after a load still sees
the old register value. Compiled code never relies on that; the generator checks every load and
emits the R3000A behaviour for any instruction that does read the loaded register.
"""
import zlib
from collections import Counter
from pathlib import Path

import rabbitizer as rz

from . import decode as _decode  # noqa: F401  (sets rabbitizer's global options)
from .functions import after_return_starts
from .irx import R_MIPS_32, R_MIPS_HI16, IrxModule, elf_irx_modules

I = rz.InstrId

# Fixed IOP link addresses. 64 KB aligned; see the IOP memory map in runtime/iop.h.
MODULES = {"989snd_Library": 0x00020000, "spiop": 0x00050000}

# Names of imported functions, by library export number (Sony's export order; see ps2sdk headers).
NAMES = {
    "loadcore": "5 FlushDcache 6 RegisterLibraryEntries 7 ReleaseLibraryEntries",
    "sysmem": "4 AllocSysMemory 5 FreeSysMemory 6 QueryMemSize 7 QueryMaxFreeMemSize "
              "8 QueryTotalFreeMemSize 14 Kprintf",
    "intrman": "4 RegisterIntrHandler 5 ReleaseIntrHandler 6 EnableIntr 7 DisableIntr "
               "17 CpuSuspendIntr 18 CpuResumeIntr 23 QueryIntrContext",
    "thbase": "4 CreateThread 5 DeleteThread 6 StartThread 7 StartThreadArgs 8 ExitThread 10 TerminateThread "
              "14 ChangeThreadPriority 20 GetThreadId 24 SleepThread 25 WakeupThread 26 iWakeupThread "
              "33 DelayThread 34 GetSystemTime 39 USec2SysClock 40 SysClock2USec",
    "thsemap": "4 CreateSema 5 DeleteSema 6 SignalSema 7 iSignalSema 8 WaitSema 9 PollSema 10 iPollSema",
    "thevent": "4 CreateEventFlag 5 DeleteEventFlag 6 SetEventFlag 7 iSetEventFlag 8 ClearEventFlag "
               "9 iClearEventFlag 10 WaitEventFlag 11 PollEventFlag",
    "sifman": "5 sceSifInit 7 sceSifSetDma 8 sceSifDmaStat 29 sceSifCheckInit 32 sceSifSetDmaIntr",
    "sifcmd": "14 sceSifInitRpc 17 sceSifRegisterRpc 19 sceSifSetRpcQueue 22 sceSifRpcLoop "
              "23 sceSifGetOtherData",
    "sysclib": "5 longjmp 12 memcpy 13 memmove 14 memset 17 bzero 22 strcmp 29 strncmp 36 strtol 40 wmemcopy",
    "stdio": "4 printf",
    "timrman": "4 AllocHardTimer 6 FreeHardTimer 20 SetTimerHandler 22 SetupHardTimer 23 StartHardTimer "
               "24 StopHardTimer",
    "ioman": "4 open 5 close 6 read 8 lseek",
    "cdvdman": "6 sceCdRead 8 sceCdGetError 10 sceCdSearchFile 11 sceCdSync 14 sceCdTrayReq 15 sceCdStop "
               "28 sceCdStatus 37 sceCdCallback 39 sceCdBreak",
    "libsd": "4 sceSdInit 5 sceSdSetParam 6 sceSdGetParam 7 sceSdSetSwitch 8 sceSdGetSwitch 9 sceSdSetAddr "
             "10 sceSdGetAddr 11 sceSdSetCoreAttr 13 sceSdNote2Pitch 17 sceSdVoiceTrans 18 sceSdBlockTrans "
             "19 sceSdVoiceTransStatus 20 sceSdBlockTransStatus 23 sceSdSetEffectAttr "
             "25 sceSdClearEffectWorkArea 26 sceSdSetTransIntrHandler",
}
NAMES = {lib: dict(zip(map(int, s.split()[0::2]), s.split()[1::2])) for lib, s in NAMES.items()}


def native_name(lib: str, num: int) -> str:
    return f"iop_imp_{lib}_{NAMES.get(lib, {}).get(num, str(num))}"


def fname(addr: int) -> str:
    return f"iop_{addr:08X}"


def label(addr: int) -> str:
    return f"L_{addr:08X}"


def decode(word: int, vram: int) -> rz.Instruction:
    return rz.Instruction(word, vram=vram, category=rz.InstrCategory.CPU)


def rs(i): return (i.getRaw() >> 21) & 31
def rt(i): return (i.getRaw() >> 16) & 31
def rd(i): return (i.getRaw() >> 11) & 31
def sa_(i): return (i.getRaw() >> 6) & 31
def imm16(i): return i.getRaw() & 0xFFFF
def simm16(i): v = imm16(i); return v - 0x10000 if v & 0x8000 else v


def R(n): return f"c->r[{n}]"
def S(n): return f"(int32_t)c->r[{n}]"


def setr(n: int, expr: str) -> str:
    return f"(void)({expr});" if n == 0 else f"c->r[{n}] = (uint32_t)({expr});"


def ea(i) -> str:
    return f"(uint32_t)(c->r[{rs(i)}] + {simm16(i)})"


LOADS = {I.cpu_lb, I.cpu_lbu, I.cpu_lh, I.cpu_lhu, I.cpu_lw, I.cpu_lwl, I.cpu_lwr}


class GenError(Exception):
    pass


# --- function discovery ------------------------------------------------------------------------

class Module:
    """A relocated module with its code split into functions."""

    def __init__(self, m: IrxModule):
        self.m = m
        lo, hi = m.text
        # Import and export tables sit at the end of .text; anything after the last one is data
        # (989snd keeps a lookup table there). Code is what precedes or lies between tables.
        tables = sorted((t.addr, t.end) for t in m.imports + m.exports)
        self.ranges, cur = [], lo
        for a, e in tables:
            if a - cur >= 16:
                self.ranges.append((cur, a))
            cur = max(cur, e)
        if not tables:
            self.ranges.append((lo, hi))
        self.instrs = {a: decode(m.word(a), a) for s, e in self.ranges for a in range(s, e, 4)}
        self.stubs = m.stubs()
        self.hi16 = {a for a, t in m.relocs if t == R_MIPS_HI16}
        self.r32 = {a for a, t in m.relocs if t == R_MIPS_32}
        self.functions: list[tuple[int, int]] = []
        self.cases: dict[int, list[int]] = {}          # jr address -> switch case labels
        self.warnings: list[str] = []

    def in_code(self, a: int) -> bool:
        return a in self.instrs

    def range_of(self, a: int) -> tuple[int, int]:
        return next((s, e) for s, e in self.ranges if s <= a < e)

    def constants(self) -> dict[int, int]:
        """Addresses built by a relocated `lui` + `addiu/ori` pair: {address of the low half: value}."""
        out = {}
        for s, e in self.ranges:
            hi: dict[int, tuple[int, int]] = {}
            for a in range(s, e, 4):
                i = self.instrs[a]
                u = i.uniqueId
                if u == I.cpu_lui and a in self.hi16:
                    hi[rt(i)] = (imm16(i) << 16, a)
                    continue
                if u in (I.cpu_addiu, I.cpu_ori) and rs(i) in hi and a - hi[rs(i)][1] <= 32:
                    lo = simm16(i) if u == I.cpu_addiu else imm16(i)
                    out[a] = (hi[rs(i)][0] + lo) & 0xFFFFFFFF
                if i.modifiesRt():
                    hi.pop(rt(i), None)
                if i.modifiesRd():
                    hi.pop(rd(i), None)
        return out

    def find_switches(self, consts: dict[int, int]) -> set[int]:
        """Switch jump tables: `sltiu t, idx, N` ... table base ... `lw r, 0(base+idx*4)` `jr r`.
        Fills self.cases and returns the addresses of the table words."""
        words = set()
        for a, i in self.instrs.items():
            if i.uniqueId != I.cpu_jr or rs(i) == 31:
                continue
            window = [b for b in range(a - 48, a, 4) if b in self.instrs]
            bases = [consts[b] for b in window if b in consts and not self.in_code(consts[b])]
            count = None
            for b in reversed(window):
                if self.instrs[b].uniqueId == I.cpu_sltiu:
                    count = imm16(self.instrs[b])
                    break
            if not bases or count is None:
                self.warnings.append(f"jr ${rs(i)} at 0x{a:08X}: no switch table found")
                continue
            base, targets = bases[-1], []
            for k in range(count):
                w = base + 4 * k
                if w not in self.r32 or not self.in_code(self.m.word(w)):
                    raise GenError(f"switch at 0x{a:08X}: table word 0x{w:08X} is not a code pointer")
                targets.append(self.m.word(w))
                words.add(w)
            self.cases[a] = targets
        return words

    def discover(self) -> None:
        m = self.m
        consts = self.constants()
        table_words = self.find_switches(consts)
        case_labels = {t for ts in self.cases.values() for t in ts}

        strong = {m.entry} | {f for t in m.exports for f in t.entries if self.in_code(f)}
        for a, i in self.instrs.items():
            if i.uniqueId == I.cpu_jal or (i.isBranch() and i.doesLink()):
                t = i.getInstrIndexAsVram() if i.uniqueId == I.cpu_jal else i.getBranchVramGeneric()
                if self.in_code(t):
                    strong.add(t)
                elif t not in self.stubs:
                    raise GenError(f"call at 0x{a:08X} to 0x{t:08X}, outside the module's code")
        strong |= {v for v in consts.values() if self.in_code(v) and v not in case_labels}
        strong |= {m.word(a) for a in self.r32 if not self.in_code(a) and a not in table_words
                   and self.in_code(m.word(a)) and m.word(a) not in case_labels}
        strong = {a for a in strong if not self.is_delay_slot(a)}

        weak = {s for s, _ in self.ranges}
        for s, e in self.ranges:
            weak |= after_return_starts([self.instrs[a] for a in range(s, e, 4)])
        for a, i in self.instrs.items():             # after an unconditional jump (tail calls)
            if i.uniqueId == I.cpu_j or (i.uniqueId == I.cpu_beq and rs(i) == 0 and rt(i) == 0):
                n = a + 8
                while self.in_code(n) and self.m.word(n) == 0:
                    n += 4
                if self.in_code(n):
                    weak.add(n)
        weak = {a for a in weak - case_labels if not self.is_delay_slot(a)}

        forced: set[int] = set()
        while True:
            keep = strong | forced
            starts = self.merge(sorted(keep | weak), keep)
            stray = set()
            for s, e in self.chunks(starts):
                for a, t, kind in self.transfers(s, e):
                    if not (s <= t < e) and t not in starts and t not in self.stubs:
                        stray.add(t)
            if not stray - forced:
                break
            forced |= stray
        self.functions = self.chunks(starts)
        for t in sorted(forced):
            self.warnings.append(f"0x{t:08X} is entered by a jump from another function; made it a start")

    def is_delay_slot(self, a: int) -> bool:
        p = self.instrs.get(a - 4)
        return p is not None and (p.isBranch() or p.isJump())

    def chunks(self, starts: list[int]) -> list[tuple[int, int]]:
        out = []
        for k, s in enumerate(starts):
            r = self.range_of(s)
            e = starts[k + 1] if k + 1 < len(starts) and starts[k + 1] < r[1] else r[1]
            out.append((s, e))
        return out

    def transfers(self, s: int, e: int):
        """Non-call control transfers out of instructions in [s, e): (address, target, kind)."""
        for a in range(s, e, 4):
            i = self.instrs[a]
            if i.isBranch() and not i.doesLink():
                yield a, i.getBranchVramGeneric(), "branch"
            elif i.uniqueId == I.cpu_j:
                yield a, i.getInstrIndexAsVram(), "j"
            elif a in self.cases:
                for t in self.cases[a]:
                    yield a, t, "case"

    def merge(self, starts: list[int], keep: set[int]) -> list[int]:
        """Drop guessed starts that cut through a function: a branch (or a `j` to something that
        is not a known function) that leaves its chunk shows the boundary is not real."""
        starts = list(starts)
        changed = True
        while changed:
            changed = False
            for s, e in self.chunks(starts):
                for a, t, kind in self.transfers(s, e):
                    if s <= t < e or not self.in_code(t) or self.range_of(t) != self.range_of(s):
                        continue
                    if kind == "j" and t in keep:
                        continue                                    # tail call
                    if t >= e:
                        drop = {x for x in starts if e <= x <= t and x not in keep}
                    else:
                        drop = {s} if s not in keep else set()
                    if drop:
                        starts = [x for x in starts if x not in drop]
                        changed = True
                        break
                if changed:
                    break
        return starts


# --- code generation ---------------------------------------------------------------------------

class Generator:
    def __init__(self, modules: list[Module]):
        self.mods = modules
        self.starts = {s for mod in modules for s, _ in mod.functions}
        self.stubs = {a: v for mod in modules for a, v in mod.stubs.items()}
        # libraries exported by a recompiled module: name -> function addresses
        self.guest_libs = {t.lib: t.entries for mod in modules for t in mod.m.exports}
        self.stats: Counter = Counter()
        self.natives: set[str] = set()

    def import_call(self, lib: str, num: int) -> str:
        if lib in self.guest_libs:
            ents = self.guest_libs[lib]
            if num >= len(ents) or ents[num] not in self.starts:
                raise GenError(f"import {lib} #{num}: not a recompiled export")
            return f"{fname(ents[num])}(c);"
        name = native_name(lib, num)
        self.natives.add(name)
        return f"{name}(c);"

    def call(self, addr: int) -> str:
        if addr in self.stubs:
            return self.import_call(*self.stubs[addr])
        if addr in self.starts:
            return f"{fname(addr)}(c);"
        self.stats["indirect call to unknown address"] += 1
        return f"iop_call(c, 0x{addr:08X}u);"


class FunctionTranslator:
    def __init__(self, gen: Generator, mod: Module, start: int, end: int):
        self.g, self.mod, self.start, self.end = gen, mod, start, end
        self.addrs = list(range(start, end, 4))
        self.labels = set()
        for a, t, kind in mod.transfers(start, end):
            if start <= t < end:
                self.labels.add(t)
        self.loop_heads = {t for a, t, kind in mod.transfers(start, end) if start <= t <= a}

    def ins(self, a: int) -> rz.Instruction:
        return self.mod.instrs[a]

    def jump(self, addr: int) -> str:
        if self.start <= addr < self.end:
            return f"goto {label(addr)};"
        return f"{{ {self.g.call(addr)} return; }}"

    @staticmethod
    def branch_cond(i) -> str:
        u = i.uniqueId
        a, b = S(rs(i)), S(rt(i))
        if u == I.cpu_beq: return f"{R(rs(i))} == {R(rt(i))}"
        if u == I.cpu_bne: return f"{R(rs(i))} != {R(rt(i))}"
        if u == I.cpu_blez: return f"{a} <= 0"
        if u == I.cpu_bgtz: return f"{a} > 0"
        if u in (I.cpu_bltz, I.cpu_bltzal): return f"{a} < 0"
        if u in (I.cpu_bgez, I.cpu_bgezal): return f"{a} >= 0"
        raise GenError(f"0x{i.vram:08X}: {i.getOpcodeName()} is not a MIPS I branch")

    def emit_branch(self, i, delay: str) -> list[str]:
        cond = self.branch_cond(i)
        t = i.getBranchVramGeneric()
        if i.doesLink():
            return [f"{{ int br = ({cond}); c->r[31] = 0x{i.vram + 8:08X}u; {delay} if (br) {{ {self.g.call(t)} }} }}"]
        return [f"{{ int br = ({cond}); {delay} if (br) {{ {self.jump(t)} }} }}"]

    def emit_jump(self, i, delay: str) -> list[str]:
        u = i.uniqueId
        if u == I.cpu_j:
            return [delay, self.jump(i.getInstrIndexAsVram())]
        if u == I.cpu_jal:
            return [f"c->r[31] = 0x{i.vram + 8:08X}u; {delay}", self.g.call(i.getInstrIndexAsVram())]
        if u == I.cpu_jalr:
            link = setr(rd(i), f"0x{i.vram + 8:08X}u")
            return [f"{{ uint32_t t = {R(rs(i))}; {link} {delay} iop_call(c, t); }}"]
        if u == I.cpu_jr:
            if rs(i) == 31:
                return [f"{delay} return;"]
            cases = self.mod.cases.get(i.vram)
            if cases:
                body = " ".join(f"case 0x{a:08X}u: goto {label(a)};" for a in sorted(set(cases)))
                return [f"{{ uint32_t t = {R(rs(i))}; {delay}",
                        f"  switch (t) {{ {body} default: iop_call(c, t); return; }} }}"]
            return [f"{{ uint32_t t = {R(rs(i))}; {delay} iop_call(c, t); return; }}"]
        raise GenError(f"0x{i.vram:08X}: unexpected jump {i.getOpcodeName()}")

    def emit(self, i) -> str:
        u, op = i.uniqueId, i.getOpcodeName()
        self.g.stats[op] += 1
        s, t, d, sa = rs(i), rt(i), rd(i), sa_(i)
        if i.getRaw() == 0 or u == I.cpu_nop:
            return "/* nop */"
        # arithmetic: the R3000A's add/addi/sub trap on overflow; compiled code uses the
        # non-trapping forms, so the trap is not modelled.
        if u in (I.cpu_addu, I.cpu_add): return setr(d, f"{R(s)} + {R(t)}")
        if u in (I.cpu_subu, I.cpu_sub): return setr(d, f"{R(s)} - {R(t)}")
        if u in (I.cpu_addiu, I.cpu_addi): return setr(t, f"{R(s)} + 0x{simm16(i) & 0xFFFFFFFF:X}u")
        if u == I.cpu_lui: return setr(t, f"0x{imm16(i) << 16:08X}u")
        if u == I.cpu_and: return setr(d, f"{R(s)} & {R(t)}")
        if u == I.cpu_or: return setr(d, f"{R(s)} | {R(t)}")
        if u == I.cpu_xor: return setr(d, f"{R(s)} ^ {R(t)}")
        if u == I.cpu_nor: return setr(d, f"~({R(s)} | {R(t)})")
        if u == I.cpu_andi: return setr(t, f"{R(s)} & 0x{imm16(i):X}u")
        if u == I.cpu_ori: return setr(t, f"{R(s)} | 0x{imm16(i):X}u")
        if u == I.cpu_xori: return setr(t, f"{R(s)} ^ 0x{imm16(i):X}u")
        if u == I.cpu_slt: return setr(d, f"{S(s)} < {S(t)}")
        if u == I.cpu_sltu: return setr(d, f"{R(s)} < {R(t)}")
        if u == I.cpu_slti: return setr(t, f"{S(s)} < {simm16(i)}")
        if u == I.cpu_sltiu: return setr(t, f"{R(s)} < 0x{simm16(i) & 0xFFFFFFFF:X}u")
        if u == I.cpu_sll: return setr(d, f"{R(t)} << {sa}")
        if u == I.cpu_srl: return setr(d, f"{R(t)} >> {sa}")
        if u == I.cpu_sra: return setr(d, f"{S(t)} >> {sa}")
        if u == I.cpu_sllv: return setr(d, f"{R(t)} << ({R(s)} & 31)")
        if u == I.cpu_srlv: return setr(d, f"{R(t)} >> ({R(s)} & 31)")
        if u == I.cpu_srav: return setr(d, f"{S(t)} >> ({R(s)} & 31)")
        if u == I.cpu_mult:
            return f"{{ int64_t p = (int64_t){S(s)} * (int64_t){S(t)}; c->lo = (uint32_t)p; c->hi = (uint32_t)((uint64_t)p >> 32); }}"
        if u == I.cpu_multu:
            return f"{{ uint64_t p = (uint64_t){R(s)} * (uint64_t){R(t)}; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}"
        if u == I.cpu_div: return f"iop_div(c, {R(s)}, {R(t)});"
        if u == I.cpu_divu: return f"iop_divu(c, {R(s)}, {R(t)});"
        if u == I.cpu_mfhi: return setr(d, "c->hi")
        if u == I.cpu_mflo: return setr(d, "c->lo")
        if u == I.cpu_mthi: return f"c->hi = {R(s)};"
        if u == I.cpu_mtlo: return f"c->lo = {R(s)};"
        if u == I.cpu_lb: return setr(t, f"(int32_t)(int8_t)iop_r8({ea(i)})")
        if u == I.cpu_lbu: return setr(t, f"iop_r8({ea(i)})")
        if u == I.cpu_lh: return setr(t, f"(int32_t)(int16_t)iop_r16({ea(i)})")
        if u == I.cpu_lhu: return setr(t, f"iop_r16({ea(i)})")
        if u == I.cpu_lw: return setr(t, f"iop_r32({ea(i)})")
        if u == I.cpu_lwl: return f"iop_lwl(c, {t}, {ea(i)});" if t else "/* lwl to $zero */"
        if u == I.cpu_lwr: return f"iop_lwr(c, {t}, {ea(i)});" if t else "/* lwr to $zero */"
        if u == I.cpu_sb: return f"iop_w8({ea(i)}, (uint8_t){R(t)});"
        if u == I.cpu_sh: return f"iop_w16({ea(i)}, (uint16_t){R(t)});"
        if u == I.cpu_sw: return f"iop_w32({ea(i)}, {R(t)});"
        if u == I.cpu_swl: return f"iop_swl(c, {t}, {ea(i)});"
        if u == I.cpu_swr: return f"iop_swr(c, {t}, {ea(i)});"
        if u == I.cpu_break: return f"iop_break(c, 0x{i.vram:08X}u, 0x{(i.getRaw() >> 6) & 0xFFFFF:X}u);"
        if u == I.cpu_syscall: return f"iop_syscall(c, 0x{i.vram:08X}u);"
        self.g.stats[f"UNIMPL {op}"] += 1
        return f"iop_unimpl(c, 0x{i.vram:08X}u, \"{op}\");"

    def load_hazard(self, a: int) -> bool:
        """True if the instruction after the load at `a` reads the register being loaded (it
        would see the old value on the R3000A)."""
        i, n = self.ins(a), self.mod.instrs.get(a + 4)
        if i.uniqueId not in LOADS or n is None or rt(i) == 0:
            return False
        if i.uniqueId in (I.cpu_lwl, I.cpu_lwr) and n.uniqueId in (I.cpu_lwl, I.cpu_lwr) and rt(n) == rt(i):
            return False                                    # lwl/lwr pairs merge, by design
        return (n.readsRs() and rs(n) == rt(i)) or (n.readsRt() and rt(n) == rt(i))

    def translate(self) -> str:
        out = [f"/* 0x{self.start:08X}-0x{self.end:08X} */", f"void {fname(self.start)}(iop_ctx* c) {{",
               f"    IOP_TRACE(0x{self.start:08X}u);"]
        k = 0
        n = len(self.addrs)
        while k < n:
            a = self.addrs[k]
            i = self.ins(a)
            asm = i.disassemble().replace("*/", "* /")
            if a in self.labels:
                out.append(f"{label(a)}:;")
                if a in self.loop_heads:
                    out.append("    IOP_POLL(c);")
            if i.isBranch() or i.isJump():
                if k + 1 >= n:
                    raise GenError(f"0x{a:08X}: branch at the end of a function")
                slot = self.ins(a + 4)
                if slot.isBranch() or slot.isJump():
                    raise GenError(f"0x{a:08X}: branch in a delay slot")
                if self.load_hazard(a + 4):
                    raise GenError(f"0x{a + 4:08X}: load in a delay slot feeds the branch target")
                delay = self.emit(slot)
                self.g.stats[i.getOpcodeName()] += 1
                lines = self.emit_branch(i, delay) if i.isBranch() else self.emit_jump(i, delay)
                out.append(f"    /* {a:08X}: {asm} */")
                out += [f"    {ln}" for ln in lines]
                if a + 4 in self.labels:                  # something jumps straight to the delay slot
                    out.append(f"    if (0) {{ {label(a + 4)}:; {delay} }}")
                k += 2
                continue
            out.append(f"    /* {a:08X}: {asm} */")
            if self.load_hazard(a):
                nxt = self.ins(a + 4)
                if nxt.isBranch() or nxt.isJump() or a + 4 in self.labels:
                    raise GenError(f"0x{a:08X}: load-delay hazard before a branch or label")
                r = rt(i)
                self.g.stats["load-delay hazard"] += 1
                out.append(f"    {{ uint32_t old = c->r[{r}]; {self.emit(i)} uint32_t loaded = c->r[{r}]; "
                           f"c->r[{r}] = old; {self.emit(nxt)} c->r[{r}] = loaded; }} /* load delay */")
                out.append(f"    /* {a + 4:08X}: {nxt.disassemble()} (in the load delay slot above) */")
                k += 2
                continue
            out.append(f"    {self.emit(i)}")
            k += 1
        nxt = f"{fname(self.end)}(c);" if self.end in self.g.starts else f"iop_call(c, 0x{self.end:08X}u);"
        out.append(f"    {nxt} /* fell off the end */")
        out.append("}")
        return "\n".join(out)


# --- driver ------------------------------------------------------------------------------------

def prepare(modules: list[tuple[IrxModule, int]]) -> list[tuple[IrxModule, Module]]:
    """Relocate each (module as shipped, link address) and split its code into functions."""
    out = [(orig, Module(orig.relocated(base))) for orig, base in modules]
    for _, mod in out:
        mod.discover()
    return out


def load_modules(elf: str, names=None) -> list[tuple[IrxModule, Module]]:
    found = {m.name: m for _, m in elf_irx_modules(elf)}
    wanted = []
    for name in names or list(MODULES):
        if name not in found:
            raise GenError(f"{elf}: no IOP module named {name}")
        wanted.append((found[name], MODULES[name]))
    return prepare(wanted)


def generate(elf: str, out_dir: str, per_file: int = 400) -> str:
    return write_c(load_modules(elf), out_dir, per_file)


def write_c(pairs: list[tuple[IrxModule, Module]], out_dir: str, per_file: int = 400) -> str:
    mods = [mod for _, mod in pairs]
    gen = Generator(mods)
    bodies = []
    for mod in mods:
        for s, e in mod.functions:
            bodies.append(FunctionTranslator(gen, mod, s, e).translate())
    # Import stubs stay callable by address (function pointers to imported functions).
    for a, (lib, num) in sorted(gen.stubs.items()):
        bodies.append(f"/* import stub {lib} #{num} */\nvoid {fname(a)}(iop_ctx* c) {{ {gen.import_call(lib, num)} }}")

    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for stale in [*out.glob("iop_funcs_*.c")]:
        stale.unlink()
    entries = sorted({s for mod in mods for s, _ in mod.functions} | set(gen.stubs))
    protos = "\n".join(f"void {fname(a)}(iop_ctx* c);" for a in entries)
    natives = "\n".join(f"void {n}(iop_ctx* c);" for n in sorted(gen.natives))
    (out / "iop_recomp.h").write_text(
        "/* Generated by `python -m recomp iop`: recompiled IOP modules. Do not edit. */\n"
        f'#pragma once\n#include "iop.h"\n\n/* native IOP libraries (runtime/iop_*.c) */\n{natives}\n\n'
        f"/* recompiled functions and import stubs */\n{protos}\n", encoding="utf-8")
    for n in range(0, len(bodies), per_file):
        chunk = "\n\n".join(bodies[n:n + per_file])
        (out / f"iop_funcs_{n // per_file:03}.c").write_text(f'#include "iop_recomp.h"\n\n{chunk}\n', encoding="utf-8")

    table = ",\n".join(f"    {{ 0x{a:08X}u, {fname(a)} }}" for a in entries)
    descs = []
    for orig, mod in pairs:
        m = mod.m
        descs.append(
            f'    {{ "{m.name}", 0x{m.base:08X}u, 0x{m.memsz:X}u, 0x{len(m.image):X}u, 0x{m.text_size:X}u, '
            f"0x{m.data_size:X}u, 0x{m.bss_size:X}u, 0x{m.entry:08X}u, 0x{m.gp:08X}u, "
            f"0x{zlib.crc32(orig.image):08X}u, 0x{zlib.crc32(m.image):08X}u, {fname(m.entry)} }}")
    (out / "iop_table.c").write_text(
        '#include "iop_recomp.h"\n\n'
        f"static const iop_func_entry funcs[] = {{\n{table}\n}};\n\n"
        f"static const iop_module_desc mods[] = {{\n" + ",\n".join(descs) + "\n};\n\n"
        "/* Registered before main(), so the runtime needs no link-time reference to this file. */\n"
        "__attribute__((constructor)) static void iop_register(void) {\n"
        "    iop_register_recompiled(funcs, sizeof funcs / sizeof funcs[0], mods, sizeof mods / sizeof mods[0]);\n"
        "}\n", encoding="utf-8")

    unimpl = sum(v for k, v in gen.stats.items() if k.startswith("UNIMPL "))
    nfuncs = sum(len(mod.functions) for mod in mods)
    lines = [f"wrote {nfuncs:,} IOP functions + {len(gen.stubs)} import stubs from "
             + ", ".join(f"{mod.m.name}@0x{mod.m.base:X}" for mod in mods)
             + f" to {out} ({unimpl} untranslated instructions, {gen.stats['load-delay hazard']} load-delay hazards)"]
    for mod in mods:
        lines += [f"  warning: {mod.m.name}: {w}" for w in mod.warnings]
    lines += [f"  warning: {k}: {v}" for k, v in gen.stats.items() if k.startswith("UNIMPL") or k.startswith("indirect")]
    return "\n".join(lines)
