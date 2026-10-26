"""r2 (modern-renderer RE) helper: annotated disassembly and cross references for SCUS_973.16.

    python scripts/r2_dis.py build                 # writes build/r2/listing.txt (+ index), ~1 min
    python scripts/r2_dis.py func <addr>           # the whole function containing addr, annotated
    python scripts/r2_dis.py callers <addr>        # jal sites calling addr (with their function)
    python scripts/r2_dis.py refs <addr> [<end>]   # instructions whose resolved constant is in [addr, end)
    python scripts/r2_dis.py vtref <addr>          # data words (in .data/.rodata) equal to addr (vtables, tables)

Annotation: lui/addiu|ori|lw|sw|lq|sq|lwc1|... pairs on the same base register are resolved to absolute
addresses (shown as `; =0x...`), per function, linear (good enough for compiler output). Reads the ELF from
the main checkout's orig/ (read only). Output under build/r2/ (not committed).
"""
import bisect
import os
import pickle
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from recomp.elf import load  # noqa: E402
from recomp.functions import find_functions  # noqa: E402

MAIN = Path(r"C:\Users\tanih\Projects\sly2-decomp")
ELF = MAIN / "orig" / "SCUS_973.16"
OUT = ROOT / "build" / "r2"
IDX = OUT / "index.pkl"

LO_OPS = {"addiu", "ori", "lw", "sw", "lh", "lhu", "sh", "lb", "lbu", "sb", "ld", "sd", "lq", "sq",
          "lwc1", "swc1", "lqc2", "sqc2", "daddiu"}


def build():
    prog = load(str(ELF))
    funcs = find_functions(prog)
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []          # (addr, text, func_start, resolved or None, jal target or None)
    for f in funcs:
        hi = {}
        for ins in f.instrs:
            a = ins.vram
            txt = ins.disassemble()
            op = ins.getOpcodeName()
            res = None
            jal = None
            try:
                if op == "jal":
                    jal = ins.getInstrIndexAsVram()
                if op in LO_OPS and ins.rs.value in hi:
                    imm = ins.getProcessedImmediate()
                    if op == "ori":
                        imm &= 0xFFFF
                    res = (hi[ins.rs.value] + imm) & 0xFFFFFFFF
                # GPR writes kill the tracked hi part (lui+addiu into the same register keeps the full address)
                if op == "lui":
                    hi[ins.rt.value] = (ins.getProcessedImmediate() & 0xFFFF) << 16
                elif op in ("addiu", "ori", "daddiu") and ins.rt.value == ins.rs.value and res is not None:
                    hi[ins.rt.value] = res
                else:
                    if ins.modifiesRt() and op not in ("lwc1", "lqc2", "swc1", "sqc2"):
                        hi.pop(ins.rt.value, None)
                    if ins.modifiesRd():
                        hi.pop(ins.rd.value, None)
                    if op in ("jal", "jalr"):
                        for r in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                            hi.pop(r, None)
            except Exception:
                pass
            rows.append((a, txt, f.start, res, jal))
        rows.append((f.end, None, f.start, None, None))
    with open(OUT / "listing.txt", "w") as fp:
        cur = None
        for a, txt, fs, res, jal in rows:
            if txt is None:
                continue
            if fs != cur:
                fp.write(f"\n=== f_{fs:08X}\n")
                cur = fs
            s = f"{a:08X}: {txt}"
            if res is not None:
                s += f"  ; =0x{res:08X}"
            fp.write(s + "\n")
    data = []
    for s in prog.sections:
        if s.name in (".rodata", ".data") and s.size:
            data.append((s.addr, prog.read_bytes(s.addr, s.addr + s.size)))
    with open(IDX, "wb") as fp:
        pickle.dump({"rows": [r for r in rows if r[1] is not None], "funcs": [(f.start, f.end) for f in funcs],
                     "data": data, "secs": [(s.name, s.addr, s.size) for s in prog.sections]}, fp)
    print(f"{len(funcs)} functions, {len(rows)} rows -> {OUT}")


def idx():
    with open(IDX, "rb") as fp:
        return pickle.load(fp)


def func_of(d, a):
    starts = [f[0] for f in d["funcs"]]
    i = bisect.bisect_right(starts, a) - 1
    return d["funcs"][i] if i >= 0 else None


def main():
    cmd = sys.argv[1]
    if cmd == "build":
        build()
        return
    d = idx()
    a = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    if cmd == "func":
        f = func_of(d, a)
        for r in d["rows"]:
            if f[0] <= r[0] < f[1]:
                s = f"{r[0]:08X}: {r[1]}"
                if r[3] is not None:
                    s += f"  ; =0x{r[3]:08X}"
                print(s)
    elif cmd == "callers":
        for r in d["rows"]:
            if r[4] == a:
                print(f"{r[0]:08X} in f_{r[2]:08X}")
    elif cmd == "refs":
        e = int(sys.argv[3], 16) if len(sys.argv) > 3 else a + 1
        for r in d["rows"]:
            if r[3] is not None and a <= r[3] < e:
                print(f"{r[0]:08X} in f_{r[2]:08X}: {r[1]}  ; =0x{r[3]:08X}")
    elif cmd == "vtref":
        for base, blob in d["data"]:
            for o in range(0, len(blob) - 3, 4):
                if int.from_bytes(blob[o:o + 4], "little") == a:
                    print(f"{base + o:08X}")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
