"""Inspect the IOP modules (IRX files) the game loads.

The game embeds its IOP modules back to back in the ELF's .irx section; IOPRP300.IMG on the disc is a
ROMDIR image of IOP kernel replacement modules. IRX files are relocatable MIPS ELFs (type 0xFF80)
with an .iopmod header, import tables (magic 0x41E00000: library name + "jr $ra; addiu $0,$0,N"
stubs) and export tables (magic 0x41C00000: library name + function pointers).

    python scripts/irx_tool.py list                       modules in the ELF and in IOPRP300.IMG
    python scripts/irx_tool.py imports [module...]        import tables with function names
    python scripts/irx_tool.py exports <module>           export table entries
    python scripts/irx_tool.py dis <module> <hex|eN> [-n 60]   disassemble (eN = export N)
    python scripts/irx_tool.py table <module> <hex> [-n 200]   relocated pointer table
    python scripts/irx_tool.py calls <module> [lib]       call sites of imported functions
    python scripts/irx_tool.py extract <dir>              write each module to <dir> (keep it in build/)

<module> is a case-insensitive substring of the module name (e.g. 989snd, spiop, sound).
Addresses are module-relative (modules are linked at 0 and relocated by the IOP loader).
Function names for import numbers follow Sony's library export order (as listed in ps2sdk headers).
"""
import argparse
import struct
import sys
from pathlib import Path

import rabbitizer as rz

from paths import ELF, ORIG

rz.config.pseudos_enablePseudos = False

IRX_VADDR, IRX_SIZE = 0x00472200, 0x73C40          # .irx section of SCUS_973.16
PT_IOPMOD = 0x70000080
R_MIPS_32 = 2

NAMES = {
    "loadcore": "3 GetLoadcoreInternalData 4 FlushIcache 5 FlushDcache 6 RegisterLibraryEntries "
                "7 ReleaseLibraryEntries 8 LinkImports 9 UnLinkImports 10 RegisterNonAutoLinkEntries "
                "11 QueryLibraryEntryTable 12 QueryBootMode 13 RegisterBootMode",
    "sysmem": "4 AllocSysMemory 5 FreeSysMemory 6 QueryMemSize 7 QueryMaxFreeMemSize "
              "8 QueryTotalFreeMemSize 9 QueryBlockTopAddress 10 QueryBlockSize 14 Kprintf",
    "intrman": "4 RegisterIntrHandler 5 ReleaseIntrHandler 6 EnableIntr 7 DisableIntr 8 CpuDisableIntr "
               "9 CpuEnableIntr 17 CpuSuspendIntr 18 CpuResumeIntr 23 QueryIntrContext",
    "thbase": "4 CreateThread 5 DeleteThread 6 StartThread 7 StartThreadArgs 8 ExitThread 9 ExitDeleteThread "
              "10 TerminateThread 11 iTerminateThread 12 DisableDispatchThread 13 EnableDispatchThread "
              "14 ChangeThreadPriority 15 iChangeThreadPriority 16 RotateThreadReadyQueue "
              "17 iRotateThreadReadyQueue 18 ReleaseWaitThread 19 iReleaseWaitThread 20 GetThreadId "
              "21 CheckThreadStack 22 ReferThreadStatus 23 iReferThreadStatus 24 SleepThread 25 WakeupThread "
              "26 iWakeupThread 27 CancelWakeupThread 28 iCancelWakeupThread 29 SuspendThread "
              "30 iSuspendThread 31 ResumeThread 32 iResumeThread 33 DelayThread 34 GetSystemTime 35 SetAlarm "
              "36 iSetAlarm 37 CancelAlarm 38 iCancelAlarm 39 USec2SysClock 40 SysClock2USec "
              "41 GetSystemStatusFlag 42 GetThreadCurrentPriority 43 GetSystemTimeLow",
    "thsemap": "4 CreateSema 5 DeleteSema 6 SignalSema 7 iSignalSema 8 WaitSema 9 PollSema 10 iPollSema "
               "11 ReferSemaStatus 12 iReferSemaStatus",
    "thevent": "4 CreateEventFlag 5 DeleteEventFlag 6 SetEventFlag 7 iSetEventFlag 8 ClearEventFlag "
               "9 iClearEventFlag 10 WaitEventFlag 11 PollEventFlag 12 ReferEventFlagStatus "
               "13 iReferEventFlagStatus",
    "sifman": "4 sceSifDma2Init 5 sceSifInit 6 sceSifSetDChain 7 sceSifSetDma 8 sceSifDmaStat 9 sceSifSetOne "
              "21 sceSifGetMSFlag 22 sceSifSetMSFlag 23 sceSifGetSMFlag 24 sceSifSetSMFlag "
              "25 sceSifGetMainAddr 26 sceSifGetSubAddr 27 sceSifSetSubAddr 28 sceSifIntrMain "
              "29 sceSifCheckInit 30 sceSifSetDmaIntrHandler 31 sceSifResetDmaIntrHandler 32 sceSifSetDmaIntr",
    "sifcmd": "4 sceSifInitCmd 5 sceSifExitCmd 6 sceSifGetSreg 7 sceSifSetSreg 8 sceSifSetCmdBuffer "
              "9 sceSifSetSysCmdBuffer 10 sceSifAddCmdHandler 11 sceSifRemoveCmdHandler 12 sceSifSendCmd "
              "13 isceSifSendCmd 14 sceSifInitRpc 15 sceSifBindRpc 16 sceSifCallRpc 17 sceSifRegisterRpc "
              "18 sceSifCheckStatRpc 19 sceSifSetRpcQueue 20 sceSifGetNextRequest 21 sceSifExecRequest "
              "22 sceSifRpcLoop 23 sceSifGetOtherData 24 sceSifRemoveRpc 25 sceSifRemoveRpcQueue",
    "sysclib": "4 setjmp 5 longjmp 6 toupper 7 tolower 8 look_ctype_table 9 get_ctype_table 10 memchr "
               "11 memcmp 12 memcpy 13 memmove 14 memset 15 bcmp 16 bcopy 17 bzero 18 prnt 19 sprintf "
               "20 strcat 21 strchr 22 strcmp 23 strcpy 24 strcspn 25 index 26 rindex 27 strlen 28 strncat "
               "29 strncmp 30 strncpy 31 strpbrk 32 strrchr 33 strspn 34 strstr 35 strtok 36 strtol 37 atob "
               "38 strtoul 40 wmemcopy 41 wmemset 42 vsprintf",
    "stdio": "4 printf",
    "timrman": "4 AllocHardTimer 5 ReferHardTimer 6 FreeHardTimer 7 SetTimerMode 8 GetTimerStatus "
               "9 SetTimerCounter 10 GetTimerCounter 11 SetTimerCompare 12 GetTimerCompare 13 SetHoldMode "
               "14 GetHoldMode 15 GetHoldReg 16 GetHardTimerIntrCode 20 SetTimerHandler 21 SetOverflowHandler "
               "22 SetupHardTimer 23 StartHardTimer 24 StopHardTimer",
    "ioman": "4 open 5 close 6 read 7 write 8 lseek 9 ioctl 10 remove 11 mkdir 12 rmdir 13 dopen 14 dclose "
             "15 dread 16 getstat 17 chstat 18 format 20 AddDrv 21 DelDrv",
    "cdvdman": "4 sceCdInit 5 sceCdStandby 6 sceCdRead 7 sceCdSeek 8 sceCdGetError 9 sceCdGetToc "
               "10 sceCdSearchFile 11 sceCdSync 12 sceCdGetDiskType 13 sceCdDiskReady 14 sceCdTrayReq "
               "15 sceCdStop 16 sceCdPosToInt 17 sceCdIntToPos 21 sceCdCheckCmd 24 sceCdReadClock "
               "28 sceCdStatus 29 sceCdApplySCmd 37 sceCdCallback 38 sceCdPause 39 sceCdBreak",
    "vblank": "4 WaitVblankStart 5 WaitVblankEnd 6 WaitVblank 7 WaitNonVblank 8 RegisterVblankHandler "
              "9 ReleaseVblankHandler",
    "modload": "5 LoadModuleAddress 6 LoadModule 7 LoadStartModule 8 StartModule 9 LoadModuleBufferAddress "
               "10 LoadModuleBuffer 13 SetCheckKelfPathCallback",
    "libsd": "2 sceSdQuit 4 sceSdInit 5 sceSdSetParam 6 sceSdGetParam 7 sceSdSetSwitch 8 sceSdGetSwitch "
             "9 sceSdSetAddr 10 sceSdGetAddr 11 sceSdSetCoreAttr 12 sceSdGetCoreAttr 13 sceSdNote2Pitch "
             "14 sceSdPitch2Note 15 sceSdProcBatch 16 sceSdProcBatchEx 17 sceSdVoiceTrans 18 sceSdBlockTrans "
             "19 sceSdVoiceTransStatus 20 sceSdBlockTransStatus 21 sceSdSetTransCallback "
             "22 sceSdSetIRQCallback 23 sceSdSetEffectAttr 24 sceSdGetEffectAttr 25 sceSdClearEffectWorkArea "
             "26 sceSdSetTransIntrHandler 27 sceSdSetSpu2IntrHandler 28 sceSdGetTransIntrHandlerArgument "
             "29 sceSdGetSpu2IntrHandlerArgument 30 sceSdStopTrans 31 sceSdCleanEffectWorkArea "
             "32 sceSdSetEffectMode 33 sceSdSetEffectModeParams",
}
NAMES = {lib: dict(zip(map(int, s.split()[0::2]), s.split()[1::2])) for lib, s in NAMES.items()}


def func_name(lib, num):
    return NAMES.get(lib, {}).get(num, f"#{num}")


class Irx:
    def __init__(self, data: bytes, where: str):
        self.data, self.where = data, where
        phoff, shoff = struct.unpack_from("<II", data, 0x1C)
        phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 0x2A)
        self.name, self.version, self.img = "?", "?", b""
        self.entry = self.gp = self.text = self.dsize = self.bss = 0
        for i in range(phnum):
            p_type, p_off, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from("<6I", data, phoff + i * phentsize)
            if p_type == PT_IOPMOD:
                _, self.entry, self.gp, self.text, self.dsize, self.bss, ver = struct.unpack_from("<6IH", data, p_off)
                self.name = data[p_off + 26:data.index(b"\0", p_off + 26)].decode("latin1")
                self.version = f"{ver >> 8}.{ver & 0xFF}"
            elif p_type == 1:
                self.img = bytearray(data[p_off:p_off + p_filesz]) + bytearray(p_memsz - p_filesz)
        self.reloc32 = set()
        for i in range(shnum):
            sh = struct.unpack_from("<10I", data, shoff + i * shentsize)
            if sh[1] == 9:                                          # SHT_REL
                for k in range(sh[5] // 8):
                    off, info = struct.unpack_from("<II", data, sh[4] + 8 * k)
                    if info & 0xFF == R_MIPS_32:
                        self.reloc32.add(off)
        self.imports, self.exports, self.stubs, self.labels = [], [], {}, {}
        for o in range(0, min(len(self.img), self.text) - 20, 4):
            magic = self.word(o)
            if magic not in (0x41E00000, 0x41C00000):
                continue
            ver = self.word(o + 8)
            lib = bytes(self.img[o + 12:o + 20]).split(b"\0")[0].decode("latin1")
            entries, p = [], o + 20
            if magic == 0x41E00000:                                 # import: jr $ra; addiu $0,$0,N
                while self.word(p) == 0x03E00008 and self.word(p + 4) >> 16 == 0x2400:
                    n = self.word(p + 4) & 0xFFFF
                    entries.append(n)
                    self.stubs[p] = (lib, n)
                    p += 8
                self.imports.append((lib, ver, entries))
            else:                                                   # export: relocated pointers
                while p in self.reloc32:
                    entries.append(self.word(p))
                    self.labels.setdefault(self.word(p), f"{lib}:{len(entries) - 1}")
                    p += 4
                self.exports.append((lib, ver, entries))

    def word(self, a):
        return struct.unpack_from("<I", self.img, a)[0] if 0 <= a <= len(self.img) - 4 else 0

    def label(self, a):
        if a in self.stubs:
            lib, n = self.stubs[a]
            return f"{lib}.{func_name(lib, n)}"
        return self.labels.get(a)


def elf_modules():
    d = ELF.read_bytes()
    off = None
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    for i in range(shnum):
        sh = struct.unpack_from("<10I", d, shoff + i * shentsize)
        if sh[3] == IRX_VADDR:
            off = sh[4]
    blob = d[off:off + IRX_SIZE]
    mods, o = [], 0
    while o < len(blob) - 4:
        if blob[o:o + 4] != b"\x7fELF":
            o += 4
            continue
        shoff, = struct.unpack_from("<I", blob, o + 0x20)
        phoff, = struct.unpack_from("<I", blob, o + 0x1C)
        phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", blob, o + 0x2A)
        end = shoff + shentsize * shnum
        for i in range(shnum):
            sh = struct.unpack_from("<10I", blob, o + shoff + i * shentsize)
            if sh[1] != 8:
                end = max(end, sh[4] + sh[5])
        mods.append(Irx(blob[o:o + end], f"EE 0x{IRX_VADDR + o:08X}"))
        o += (end + 3) & ~3
    return mods


def ioprp_modules(path=ORIG / "IOPRP300.IMG"):
    if not path.exists():
        return []
    d = path.read_bytes()
    entries, o = [], 0
    while d[o:o + 10].strip(b"\0"):
        name = d[o:o + 10].split(b"\0")[0].decode("latin1")
        _, size = struct.unpack_from("<HI", d, o + 10)
        entries.append((name, size))
        o += 16
    mods, off = [], 0
    for name, size in entries:
        if d[off:off + 4] == b"\x7fELF":
            m = Irx(d[off:off + size], f"IOPRP {name}")
            mods.append(m)
        off += (size + 15) & ~15
    return mods


def find(mods, key):
    hits = [m for m in mods if key.lower() in m.name.lower()]
    if not hits:
        sys.exit(f"no module matching {key!r}")
    return hits[0]


def disasm(m, start, count):
    hi = {}
    for i in range(count):
        a = start + 4 * i
        if a + 4 > len(m.img):
            break
        w = m.word(a)
        if a in m.labels:
            print(f"            <{m.labels[a]}>")
        text = rz.Instruction(w, vram=a, category=rz.InstrCategory.CPU).disassemble()
        op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        note = ""
        if op in (2, 3):
            t = (w & 0x3FFFFFF) << 2
            note = f"-> 0x{t:X}" + (f" <{m.label(t)}>" if m.label(t) else "")
        elif op == 0x0F:
            hi[rt] = imm << 16
        elif op in (0x09, 0x0D) and rs in hi:
            v = (hi[rs] + simm) & 0xFFFFFFFF if op == 0x09 else hi[rs] | imm
            note = f"= 0x{v:08X}" + (f" <{m.label(v)}>" if m.label(v) else "")
        elif op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B) and rs in hi:
            note = f"[0x{(hi[rs] + simm) & 0xFFFFFFFF:08X}]"
        print(f"  {a:06X}: {w:08X}  {text:40s} {note}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd", choices=["list", "imports", "exports", "dis", "table", "calls", "extract"])
    p.add_argument("args", nargs="*")
    p.add_argument("-n", type=int, default=60)
    a = p.parse_args()
    mods = elf_modules()

    if a.cmd == "list":
        for m in mods:
            print(f"{m.where}  {len(m.data):6d} bytes  {m.name:22s} v{m.version:5s} text 0x{m.text:05X} "
                  f"data 0x{m.dsize:04X} bss 0x{m.bss:04X} entry 0x{m.entry:05X}")
        for m in ioprp_modules():
            exp = ", ".join(f"{lib} ({len(e)})" for lib, _, e in m.exports)
            print(f"{m.where:16s} {m.name:24s} v{m.version:5s} exports {exp or '-'}")
    elif a.cmd == "imports":
        for m in mods:
            if a.args and not any(k.lower() in m.name.lower() for k in a.args):
                continue
            print(f"== {m.name} {m.version}")
            for lib, ver, nums in m.imports:
                print(f"  {lib:9s} v{ver >> 8}.{ver & 0xFF}: " + ", ".join(f"{n} {func_name(lib, n)}" for n in nums))
    elif a.cmd == "exports":
        m = find(mods, a.args[0])
        for lib, ver, ptrs in m.exports:
            print(f"{lib} v{ver >> 8}.{ver & 0xFF}: {len(ptrs)} entries")
            for i, f in enumerate(ptrs):
                print(f"  {i:3d} 0x{f:05X}")
    elif a.cmd == "dis":
        m = find(mods, a.args[0])
        s = a.args[1]
        if s.startswith("e"):
            start = m.exports[0][2][int(s[1:])]
        else:
            start = int(s, 16)
        disasm(m, start, a.n)
    elif a.cmd == "table":
        m = find(mods, a.args[0])
        start = int(a.args[1], 16)
        for i in range(a.n):
            addr = start + 4 * i
            if addr not in m.reloc32:
                break
            v = m.word(addr)
            print(f"  [{i:3d}] 0x{addr:05X}: 0x{v:05X} {m.label(v) or ''}")
    elif a.cmd == "calls":
        m = find(mods, a.args[0])
        lib = a.args[1] if len(a.args) > 1 else None
        for addr in range(0, m.text, 4):
            w = m.word(addr)
            t = (w & 0x3FFFFFF) << 2
            if w >> 26 == 3 and t in m.stubs and (lib is None or m.stubs[t][0] == lib):
                print(f"  {addr:06X}: {m.label(t)}")
    elif a.cmd == "extract":
        out = Path(a.args[0])
        out.mkdir(parents=True, exist_ok=True)
        for i, m in enumerate(mods):
            (out / f"{i:02d}_{m.name}.irx").write_bytes(m.data)
        print(f"wrote {len(mods)} modules to {out}")


if __name__ == "__main__":
    main()
