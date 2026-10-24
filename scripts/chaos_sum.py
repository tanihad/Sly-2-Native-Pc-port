"""Summarise one chaos-benchmark run (scripts/chaos_bench.ps1): frame times from PS2_FPS_TRACE, per-stage ms from
PS2_PERF / PS2_GS_HW_STATS lines of run.log.

    python scripts/chaos_sum.py <run dir> [--from <s>] [--to <s>] [--name <label>] [--json <file>]

Frame time = the interval between two consecutive VBlanks on the EE thread (above 60 fps a VBlank is a game frame; the
pipeline behind it holds at most 2 frames, so this is the rate the player sees). Inside the window [from, to] (seconds
of the trace's wall clock since the start):
  fps median = 1000 / median frame time; 1% low = 1000 / 99th percentile frame time; 0.1% low = 1000 / 99.9th percentile
  (the "percentile" definition of PresentMon / CapFrameX); avg = frames / window time; worst = 1000 / longest frame.
Per stage (per game frame, ms): worker busy (PS2_PERF, per second), GS thread (mean per stats window), frontend
median and the windows' max, render CPU, GPU median and max (PS2_GS_HW_STATS windows of 60 VBlanks); the median of the
windows and the 90th percentile of the windows' values ("p90").
"""
import json
import re
import statistics
import sys
from pathlib import Path

STATS = re.compile(r"stats v(\d+): ([\d.]+) flips/s \(([\d.]+) pictures/s.*?GS thread ([\d.]+) \(mean\) \| frontend (?:thread|\(GS thread\)) "
                   r"([\d.]+) / ([\d.]+) \| render CPU ([\d.]+) / ([\d.]+), wall ([\d.]+) / ([\d.]+), present ([\d.]+) \| GPU ([\d.]+) / ([\d.]+)"
                   r".*?per VBlank: (\d+) prims, (\d+) draws")
PERF = re.compile(r"\[perf\]\s+([\d.]+)s vblanks .*?busy ([\d.]+) ms/vblank.*?\| worker busy ([\d.]+) ms/vblank")


def pct(a, q):
    if not a:
        return 0.0
    s = sorted(a)
    k = q * (len(s) - 1)
    i = int(k)
    return s[i] if i + 1 >= len(s) else s[i] + (s[i + 1] - s[i]) * (k - i)


def med(a):
    return statistics.median(a) if a else 0.0


def table(path):
    """medians over the repeats per scene and build of a chaos_bench .jsonl"""
    rows = [json.loads(l) for l in open(path) if l.strip()]
    g = {}
    for r in rows:
        if "fps_med" not in r:
            continue
        sc, rest = r["name"].split("@", 1)
        g.setdefault((sc, rest.split("#")[0]), []).append(r)
    keys = ["fps_med", "low1", "low01", "worker", "gs", "fr", "fr_p90", "rc", "gpu", "prims"]
    print("scene | build | runs | fps med | 1% low | 0.1% low | worker | GS | front (p90) | render | GPU | prims")
    for (sc, b), rs in sorted(g.items()):
        m = {k: med([x.get(k, 0) for x in rs]) for k in keys}
        print(f"{sc} | {b} | {len(rs)} | {m['fps_med']:.1f} | {m['low1']:.1f} | {m['low01']:.1f} | {m['worker']:.2f} | {m['gs']:.2f} | "
              f"{m['fr']:.2f} ({m['fr_p90']:.2f}) | {m['rc']:.2f} | {m['gpu']:.2f} | {m['prims']:.0f}")


def main():
    a = sys.argv[1:]
    if a and a[0] == "--table":
        table(a[1])
        return
    d = Path(a[0])
    t_from, t_to, name, js = 0.0, 1e9, d.name, None
    i = 1
    while i < len(a):
        if a[i] == "--from": t_from = float(a[i + 1]); i += 2
        elif a[i] == "--to": t_to = float(a[i + 1]); i += 2
        elif a[i] == "--name": name = a[i + 1]; i += 2
        elif a[i] == "--json": js = a[i + 1]; i += 2
        else: i += 1
    tr = []
    for line in open(d / "trace.txt", encoding="latin-1"):
        f = line.split()
        if len(f) >= 2:
            try: tr.append((int(f[0]), float(f[1])))
            except ValueError: pass
    vt = {v: t for v, t in tr}
    ft = []
    for (v0, t0), (v1, t1) in zip(tr, tr[1:]):
        if v1 == v0 + 1 and t0 >= t_from and t1 <= t_to:
            ft.append((t1 - t0) * 1000.0)
    out = {"name": name, "frames": len(ft)}
    if ft:
        span = sum(ft) / 1000.0
        out.update(fps_med=1000.0 / med(ft), low1=1000.0 / pct(ft, 0.99), low01=1000.0 / pct(ft, 0.999),
                   avg=len(ft) / span, worst=1000.0 / max(ft), ft_med=med(ft), ft_p99=pct(ft, 0.99), ft_p999=pct(ft, 0.999),
                   ft_max=max(ft), secs=span)
    st = {k: [] for k in ("gs", "fr", "frmax", "rc", "gpu", "gpumax", "prims", "draws")}
    wk = []
    log = d / "run.log"
    for line in open(log, encoding="latin-1"):
        m = STATS.search(line)
        if m:
            v = int(m.group(1))
            t = vt.get(v)
            if t is None or t < t_from or t > t_to or float(m.group(4)) == 0:
                continue
            st["gs"].append(float(m.group(4))); st["fr"].append(float(m.group(5))); st["frmax"].append(float(m.group(6)))
            st["rc"].append(float(m.group(7))); st["gpu"].append(float(m.group(12))); st["gpumax"].append(float(m.group(13)))
            st["prims"].append(float(m.group(14))); st["draws"].append(float(m.group(15)))
            continue
        m = PERF.search(line)
        if m and t_from <= float(m.group(1)) <= t_to:
            wk.append(float(m.group(3)))
    for k, v in st.items():
        out[k] = med(v)
        out[k + "_p90"] = pct(v, 0.9)
    out["worker"] = med(wk)
    out["worker_p90"] = pct(wk, 0.9)
    if ft:
        print(f"{name}: fps med {out['fps_med']:.1f} | 1% low {out['low1']:.1f} | 0.1% low {out['low01']:.1f} | avg {out['avg']:.1f} | "
              f"worst {out['worst']:.1f} ({len(ft)} frames, {out['secs']:.1f} s) | ms med/p90: worker {out['worker']:.2f}/{out['worker_p90']:.2f} "
              f"GS {out['gs']:.2f}/{out['gs_p90']:.2f} front {out['fr']:.2f}/{out['fr_p90']:.2f} (max {out['frmax_p90']:.2f}) "
              f"render {out['rc']:.2f}/{out['rc_p90']:.2f} GPU {out['gpu']:.2f}/{out['gpu_p90']:.2f} (max {out['gpumax_p90']:.2f}) | "
              f"prims {out['prims']:.0f} draws {out['draws']:.0f}")
    else:
        print(f"{name}: no frames in [{t_from}, {t_to}]")
    if js:
        with open(js, "a") as f:
            f.write(json.dumps(out) + "\n")


main()
