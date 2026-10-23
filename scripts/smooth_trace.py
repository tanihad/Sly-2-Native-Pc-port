"""Frame-time summary of runs with PS2_PRESENT_TRACE (render thread: presents, GPU times, shown presents) and
PS2_FPS_TRACE (EE: one line per VBlank with the game clock's dt) - notes/wip/pc-smooth.md, notes/frame-rate.md.

    python scripts/smooth_trace.py <run dir>[=label] [...] [--from S] [--to S] [--target HZ] [--png out.png]

Reads <dir>/present.txt and <dir>/trace.txt (either may be missing). Prints, for the window [from, to] seconds after
the first present: Present-call intervals (median, p95, p99, max, count > 1.5x the target period = "stutters"), the
intervals between shown presents (window runs: DXGI frame statistics), the game's dt (the EE frame time as the game
used it), GPU ms per picture, scale changes. --png: one panel per run, the intervals over time (shown ones when the
run has them, else the Present calls), a line at 1.5x the target; pure Python (no plotting packages in the venv).
"""
import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pace_plot import glyph  # noqa: E402  (3x5 font)


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    i = int(q * (len(s) - 1) + 0.5)
    return s[min(max(i, 0), len(s) - 1)]


def summary(name, v, target_ms=None):
    if not v:
        return f"{name}: none"
    s = f"{name}: n {len(v)}, median {pct(v, .5):.2f}, p95 {pct(v, .95):.2f}, p99 {pct(v, .99):.2f}, max {max(v):.2f} ms"
    if target_ms:
        over = sum(1 for x in v if x > 1.5 * target_ms)
        s += f"; > 1.5x ({1.5 * target_ms:.1f} ms): {over} ({100.0 * over / len(v):.2f}%)"
    return s


def analyse(d, t_from, t_to, target_hz, name):
    target_ms = 1000.0 / target_hz
    pres, gpu, shown = [], {}, []
    pt = os.path.join(d, "present.txt")
    if os.path.exists(pt):
        for line in open(pt):
            f = line.split()
            if not f:
                continue
            try:
                if f[0] == "p" and len(f) >= 10:
                    pres.append((int(f[1]), float(f[2]), float(f[3]), float(f[4]), float(f[5]), float(f[6]), float(f[7]),
                                 int(f[8]), int(f[9])))
                elif f[0] == "g" and len(f) >= 4:
                    gpu[int(f[1])] = float(f[2])
                elif f[0] == "s" and len(f) >= 4:
                    shown.append((int(f[1]), int(f[2]), float(f[3])))
            except ValueError:
                pass
    out = []
    t0 = pres[0][4] if pres else 0.0
    sel = [p for p in pres if t_from <= (p[4] - t0) / 1000.0 <= t_to]
    calls = [p for p in sel if p[5] > 0]
    iv = [calls[i][5] - calls[i - 1][5] for i in range(1, len(calls))]
    ivt = [(calls[i][5] - t0) / 1000.0 for i in range(1, len(calls))]
    secs = (calls[-1][5] - calls[0][5]) / 1000.0 if len(calls) > 1 else 0
    out.append(f"== {name}: {len(calls)} presents in {secs:.1f} s ({len(calls) / secs if secs else 0:.1f}/s), "
               f"skipped {sum(1 for p in sel if p[8] & 1)}, waitable timeouts {sum(1 for p in sel if p[8] & 8)}")
    out.append(summary("Present-call interval", iv, target_ms))
    out.append(summary("VBlank -> Present call", [p[5] - p[1] for p in calls if p[1] > 0]))
    out.append(summary("GPU ms per picture", [gpu[p[0]] for p in sel if p[0] in gpu and gpu[p[0]] >= 0]))
    scales = []
    for p in sel:
        if not scales or scales[-1][1] != p[7]:
            scales.append(((p[4] - t0) / 1000.0, p[7]))
    out.append(f"scale changes ({len(scales) - 1}): " + ", ".join(f"{t:.1f}s->{s}x" for t, s in scales[:30]))
    siv, sivt = [], []
    sh = [s for s in shown if t_from <= (s[2] - t0) / 1000.0 <= t_to]
    for i in range(1, len(sh)):
        if sh[i][0] == sh[i - 1][0] + 1:
            siv.append(sh[i][2] - sh[i - 1][2])
            sivt.append((sh[i][2] - t0) / 1000.0)
    if siv:
        out.append(summary("shown interval (display)", siv, target_ms))
        hist = {}
        for x in siv:
            k = round(x)
            hist[k] = hist.get(k, 0) + 1
        out.append("shown intervals (ms: count): " + ", ".join(f"{k}: {hist[k]}" for k in sorted(hist)))
    tr = os.path.join(d, "trace.txt")
    if os.path.exists(tr):
        rows = []
        for line in open(tr):
            f = line.split()
            if len(f) >= 4:
                try:
                    rows.append((int(f[0]), float(f[1]), float(f[2]), float(f[3])))
                except ValueError:
                    pass
        fr, last_t = [], None              # game frames: VBlanks whose game time advanced
        for r in rows:
            if last_t is not None and r[2] != last_t:
                fr.append(r)
            last_t = r[2]
        # (the EE trace's wall clock starts at the process start, the presents' at the first present: ~1 s apart)
        fsel = [r for r in fr if t_from <= r[1] <= t_to]
        dts = [r[3] * 1000.0 for r in fsel]
        out.append(summary("game dt (EE frame)", dts, target_ms))
        out.append(summary("game frame wall interval", [(fsel[i][1] - fsel[i - 1][1]) * 1000.0 for i in range(1, len(fsel))],
                           target_ms))
    return out, (sivt, siv) if siv else (ivt, iv), target_ms


def png(path, panels, width=900, ph=150):
    H = ph * len(panels)
    W = width
    px = bytearray(b"\xff" * (W * H * 3))

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            o = (y * W + x) * 3
            px[o:o + 3] = bytes(c)

    def text(x, y, s, c=(0, 0, 0)):
        for ch in s:
            g = glyph(ch)
            for r in range(5):
                for k in range(3):
                    if g[r * 3 + k] == "1":
                        for dy in range(2):
                            for dx in range(2):
                                put(x + k * 2 + dx, y + r * 2 + dy, c)
            x += 8

    M = 6
    for p, (label, (ts, vs), target_ms) in enumerate(panels):
        y0 = p * ph
        ymax = 4.0 * target_ms

        def ymap(ms):
            return y0 + ph - M - int((ph - 2 * M) * min(ms, ymax) / ymax)
        for x in range(W):
            put(x, y0, (120, 120, 120))
        for x in range(0, W, 2):
            put(x, ymap(target_ms), (0, 160, 0))
            put(x, ymap(1.5 * target_ms), (220, 150, 0))
        if ts:
            t_lo, t_hi = ts[0], ts[-1]
            for t, v in zip(ts, vs):
                x = M + int((W - 2 * M) * (t - t_lo) / max(1e-6, t_hi - t_lo))
                y = ymap(v)
                col = (210, 40, 40) if v > 1.5 * target_ms else (30, 60, 200)
                for dy in (0, 1):
                    for dx in (0, 1):
                        put(x + dx, y + dy, col)
        over = sum(1 for v in vs if v > 1.5 * target_ms)
        text(M + 2, y0 + 4, f"{label}  n {len(vs)}  >1.5x {over}  max {max(vs) if vs else 0:.1f}ms")
    raw = b"".join(b"\x00" + bytes(px[y * W * 3:(y + 1) * W * 3]) for y in range(H))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--from", dest="t_from", type=float, default=0.0)
    ap.add_argument("--to", dest="t_to", type=float, default=1e9)
    ap.add_argument("--target", type=float, default=59.94)
    ap.add_argument("--png")
    a = ap.parse_args()
    panels = []
    for spec in a.dirs:
        d, _, label = spec.partition("=")
        lines, series, target_ms = analyse(d, a.t_from, a.t_to, a.target, label or os.path.basename(d.rstrip("\\/")))
        print("\n".join(lines))
        panels.append((label or os.path.basename(d.rstrip("\\/")), series, target_ms))
    if a.png:
        png(a.png, panels)
        print(f"{a.png}: {len(panels)} panels")


if __name__ == "__main__":
    main()
