"""Explain why an address is or isn't a function start.  Usage: debug_start.py <elf> <hex addr>"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from recomp import functions as F  # noqa: E402
from recomp.decode import decode_range  # noqa: E402
from recomp.elf import load  # noqa: E402

prog = load(sys.argv[1])
target = int(sys.argv[2], 16)
lo, hi = next((a, b) for a, b in prog.code_ranges() if a <= target < b)
instrs = decode_range(prog.read_bytes(lo, hi), lo)
words = {i.vram: i.getRaw() for i in instrs}

referenced = F.address_constants(instrs)
holders = []
for s in prog.sections:
    if not s.executable and s.size and s.name in (".data", ".rodata", ".sdata"):
        data = prog.read_bytes(s.addr, s.addr + s.size)
        for k in range(0, len(data) - 3, 4):
            if int.from_bytes(data[k:k + 4], "little") == target:
                holders.append(f"{s.name}+0x{k:X} (0x{s.addr + k:08X})")
tables = [t for t in F.jump_table_ranges(prog, lo, hi, words) if t[0] < target <= t[1]]

print(f"address constant in code : {target in referenced}")
print(f"pointer stored in data   : {holders[:5]}")
print(f"inside a 'jump table'    : {[(hex(a), hex(b)) for a, b in tables]}")
print(f"after jr $ra             : {target in F.after_return_starts(instrs)}")
print(f"looks like function start: {F.looks_like_function_start(words, target)}")
final = {f.start for f in F.find_functions(prog)}
print(f"final function start     : {target in final}")
