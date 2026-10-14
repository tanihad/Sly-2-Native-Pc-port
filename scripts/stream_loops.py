# Stream loop check on a PS2_LOG_SD_VOICES log. For voices whose LSAX software writes (989snd's
# streams), report each loop jump that did not go where the last LSAX write said:
#   "overridden": LSAX was written since the previous jump, but the voice jumped elsewhere
#   "late": no LSAX write since the previous jump and the voice jumped back inside the same half
# plus 989snd error reports (PS2_IOP_WATCH=22140, debug builds).
# usage: python scripts\stream_loops.py <run.log> [max lines]
import re, sys
jump = re.compile(r"\[sd +([0-9.]+)\] c(\d) v(\d+) +end\+repeat at ([0-9A-F]+) -> ([0-9A-F]+)")
lsax = re.compile(r"\[sd +([0-9.]+)\] LSAX +c(\d) v(\d+) +([0-9A-F]+)")
kon = re.compile(r"\[sd +([0-9.]+)\] KON +c(\d) v(\d+)")
watch = re.compile(r"\[iop-watch +([0-9.]+)\] 22140 a0=([0-9A-F]+) a1=([0-9A-F]+)")
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 60
last_lsax = {}      # voice -> (time, addr, used)
last_jump = {}
counts = {"ok": 0, "overridden": 0, "late": 0}
shown = 0
def show(s):
    global shown
    if shown < limit:
        print(s)
    shown += 1
for line in open(sys.argv[1], errors="replace"):
    m = jump.search(line)
    if m:
        t, c, v, a, b = float(m[1]), m[2], int(m[3]), int(m[4], 16), int(m[5], 16)
        key = (c, v)
        if a == b or key not in last_lsax:
            continue
        lt, la, used = last_lsax[key]
        if not used:
            last_lsax[key] = (lt, la, True)
            if b == la:
                counts["ok"] += 1
            else:
                counts["overridden"] += 1
                show(f"{t:9.3f} c{c} v{v:<2} overridden: {a:05X} -> {b:05X}, LSAX {la:05X} written at {lt:.3f}")
        elif 0 < a - b < 0x1000:
            counts["late"] += 1
            lj = last_jump.get(key)
            show(f"{t:9.3f} c{c} v{v:<2} late: {a:05X} -> {b:05X} (no LSAX since the jump at {lj[0]:.3f} {lj[1]:05X}->{lj[2]:05X})"
                 if lj else f"{t:9.3f} c{c} v{v:<2} late: {a:05X} -> {b:05X}")
        last_jump[key] = (t, a, b)
        continue
    m = lsax.search(line)
    if m:
        last_lsax[(m[2], int(m[3]))] = (float(m[1]), int(m[4], 16), False)
        continue
    m = kon.search(line)
    if m:
        last_lsax.pop((m[2], int(m[3])), None)
        last_jump.pop((m[2], int(m[3])), None)
        continue
    m = watch.search(line)
    if m:
        show(f"{float(m[1]):9.3f} 989snd error 0x{m[2]} stream {m[3]}")
print(counts, f"({shown} events)")
