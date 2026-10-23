"""Summarise a high-frame-rate run (pc-fps2; scripts/fps2_scene.ps1): run.log with PS2_PERF and PS2_GS_HW_STATS.

    python scripts/fps2_sum.py <run.log> [--from <s>] [--name <label>] [--csv <file>]

From the second --from on: game frames per second (PS2_PERF flips/s: median, 10% low, min), the EE thread's split
(PS2_PERF categories, % of wall time; busy = not idle / not waiting for the GS thread), and from the GPU renderer's
stats lines the per-game-frame means of the GS thread, frontend thread, render CPU and GPU (medians of the per-second
values). One line per run, so runs can be compared side by side.
"""
import re
import statistics
import sys

PERF = re.compile(r"\[perf\]\s+([\d.]+)s vblanks (\d+)/(\d+) due \(([\d.]+)/s\), flips (\d+) \(([\d.]+)/s\), busy ([\d.]+) ms/vblank(.*?)\|(.*)$")
STATS = re.compile(r"stats v(\d+): ([\d.]+) flips/s \(([\d.]+) pictures/s.*?GS thread ([\d.]+) \(mean\) \| frontend (?:thread|\(GS thread\)) ([\d.]+) / ([\d.]+) \| "
                   r"render CPU ([\d.]+) / ([\d.]+), wall ([\d.]+) / ([\d.]+), present ([\d.]+) \| GPU ([\d.]+) / ([\d.]+)")
VU = re.compile(r"VU (\d+)k pairs in (\d+) programs/vblank \(([\d.]+) ns per pair")
EXTRA = re.compile(r"\[fps2\] (.*)$")


def med(a):
    return statistics.median(a) if a else 0.0


def low(a, q=0.1):
    if not a:
        return 0.0
    s = sorted(a)
    return s[int(q * (len(s) - 1))]


def main():
    args = sys.argv[1:]
    path = args[0]
    t_from = 0.0
    name = path
    csv = None
    i = 1
    while i < len(args):
        if args[i] == "--from":
            t_from = float(args[i + 1]); i += 2
        elif args[i] == "--name":
            name = args[i + 1]; i += 2
        elif args[i] == "--csv":
            csv = args[i + 1]; i += 2
        else:
            i += 1
    fps, busy, cats, vu = [], [], {}, []
    wbusy, wcats = [], {}
    igame, idist = [], []
    gs, fr, rc, gpu, sfl = [], [], [], [], []
    started = False
    extra = []
    for line in open(path, encoding="latin-1"):
        m = PERF.search(line)
        if m:
            t = float(m.group(1))
            if t < t_from:
                continue
            started = True
            fps.append(float(m.group(6)))
            busy.append(float(m.group(7)))
            ee, _, wk = m.group(9).partition(" | worker busy ")
            for c in re.finditer(r"(\w+) ([\d.]+)%", ee):
                cats.setdefault(c.group(1), []).append(float(c.group(2)))
            if wk:
                w = re.match(r"([\d.]+) ms/vblank:(.*)", wk)
                if w:
                    wbusy.append(float(w.group(1)))
                    for c in re.finditer(r"(\w+) ([\d.]+)%", w.group(2)):
                        wcats.setdefault(c.group(1), []).append(float(c.group(2)))
            v = VU.search(m.group(8))
            if v:
                vu.append(int(v.group(1)))
            continue
        if not started:
            continue
        m = STATS.search(line)
        if m:
            if float(m.group(4)) == 0:           # (a stats line without game frames: a second line of the same VBlank)
                continue
            sfl.append(float(m.group(3)))
            gs.append(float(m.group(4)))
            fr.append(float(m.group(5)))
            rc.append(float(m.group(7)))
            gpu.append(float(m.group(12)))
            continue
        m = EXTRA.search(line)
        if m:
            extra.append(m.group(1))
            d = re.search(r"interp: ([\d.]+) game frames/s \+ ([\d.]+) in-between = ([\d.]+) distinct", line)
            if d:
                igame.append(float(d.group(1)))
                idist.append(float(d.group(3)))
    if not fps:
        print(f"{name}: no PS2_PERF lines after {t_from} s")
        return
    split = " ".join(f"{k} {med(v):.0f}%" for k, v in cats.items())
    if wbusy:
        split += f" | worker busy {med(wbusy):.2f} ms/frame: " + " ".join(f"{k} {med(v):.0f}%" for k, v in wcats.items())
    print(f"{name}: fps med {med(fps):.1f} p10 {low(fps):.1f} min {min(fps):.1f} ({len(fps)} s) | EE busy {med(busy):.2f} ms/frame | {split}"
          f" | VU {med(vu):.0f}k pairs/frame | per frame ms: GS thread {med(gs):.2f} frontend {med(fr):.2f} render CPU {med(rc):.2f}"
          f" GPU {med(gpu):.2f} | pictures/s {med(sfl):.1f} p10 {low(sfl):.1f}")
    if idist:
        print(f"    interpolation: game frames/s med {med(igame):.1f} p10 {low(igame):.1f}, distinct pictures/s med {med(idist):.1f} "
              f"p10 {low(idist):.1f}")
    elif extra:
        print("   ", extra[-1])
    if csv:
        with open(csv, "a") as f:
            f.write(f"{name},{med(fps):.1f},{low(fps):.1f},{min(fps):.1f},{med(busy):.2f},{med(gs):.2f},{med(fr):.2f},{med(rc):.2f},{med(gpu):.2f}\n")


main()
