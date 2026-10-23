"""Frame-rate test analysis (pc-fps, notes/frame-rate.md): runs made with scripts/fps_run.ps1.

    python scripts/fps_trace.py <run dir> [<run dir> ...]

Reads <dir>/trace.txt (one line per VBlank: VBlank, wall s, game t, game dt, player x y z: PS2_FPS_TRACE) and the scripted
presses in <dir>/run.log, and prints per run:
  - frames per second (game frames = VBlanks in which the game clock moved) and VBlanks per second;
  - the game clock against the wall clock while it runs (1.000 = real time);
  - the game's dt (median, 5% / 95%) and how many frames sat at its 1/30 s clamp;
  - for every scripted press: what the player did until the next press - game seconds, horizontal distance and speed,
    the highest point above the start (jump height), when it was reached and when the player was back down.
Runs of the same scenario at different frame rates should give the same speeds, heights and times.
"""
import math
import re
import sys
from pathlib import Path

PRESS = re.compile(r"pad frame (\d+) \(VBlank (\d+)\): (\S+) \(")


def load(d):
    rows = []
    for line in (Path(d) / "trace.txt").read_text(encoding="latin-1").splitlines():
        p = line.split()
        if len(p) in (7, 10, 11):
            try:
                cam = tuple(float(x) for x in p[7:10]) if len(p) >= 10 else (0.0, 0.0, 0.0)
                h = float(p[10]) if len(p) == 11 and float(p[10]) != 0 else float(p[6])   # (jump height signal)
                rows.append((int(p[0]), float(p[1]), float(p[2]), float(p[3]), float(p[4]), float(p[5]), h) + cam)
            except ValueError:
                pass
    presses = []
    log = Path(d) / "run.log"
    if log.exists():
        for line in log.read_text(encoding="latin-1").splitlines():
            m = PRESS.search(line)
            if m:
                presses.append((int(m.group(2)), m.group(3)))
    return rows, presses


def clock(rows):
    """game frames, wall seconds and game seconds while the clock runs (stops of > 0.25 s split the spans)"""
    frames = wall = game = 0.0
    vbl = 0
    span0 = None
    last_move = None
    for a, b in zip(rows, rows[1:]):
        if b[2] != a[2]:
            frames += 1
            if span0 is None:
                span0 = a
            last_move = b
        elif last_move is not None and b[1] - last_move[1] > 0.25:   # stopped (pause, loading): close the span
            wall += last_move[1] - span0[1]
            game += last_move[2] - span0[2]
            span0 = last_move = None
        if span0 is not None:
            vbl += 1
    if span0 is not None and last_move is not None:
        wall += last_move[1] - span0[1]
        game += last_move[2] - span0[2]
    return frames, vbl, wall, game


def report(d):
    rows, presses = load(d)
    if len(rows) < 100:
        print(f"{d}: too few samples ({len(rows)})")
        return
    frames, vbl, wall, game = clock(rows)
    dts = sorted(r[3] for a, r in zip(rows, rows[1:]) if r[2] != a[2] and r[3] > 0)
    print(f"{Path(d).name}: {frames / wall if wall else 0:.1f} game frames/s ({vbl / wall if wall else 0:.1f} VBlanks/s) over "
          f"{wall:.1f} s of running clock; game time / wall time {game / wall if wall else 0:.4f}; dt median "
          f"{dts[len(dts) // 2] * 1000:.2f} ms (p5 {dts[len(dts) // 20] * 1000:.2f}, p95 {dts[len(dts) * 19 // 20] * 1000:.2f}), "
          f"{sum(x >= 0.0333 for x in dts) / max(1, len(dts)) * 100:.1f}% at the 1/30 s clamp")
    idx = {r[0]: i for i, r in enumerate(rows)}
    for k, (v, name) in enumerate(presses):
        i0 = idx.get(v)
        if i0 is None:
            continue
        vend = presses[k + 1][0] if k + 1 < len(presses) else rows[-1][0]
        i1 = idx.get(vend, len(rows) - 1)
        seg = rows[i0:i1 + 1]
        if len(seg) < 2:
            continue
        r0 = seg[0]
        dist = sum(math.hypot(b[4] - a[4], b[5] - a[5]) for a, b in zip(seg, seg[1:]))
        dt_game = seg[-1][2] - r0[2]
        zmax = max(seg, key=lambda r: r[6])
        down = next((r for r in seg if r[2] > zmax[2] and r[6] <= r0[6] + 1.0), None)
        line = (f"  v{v} {name:7s}: {dt_game:6.2f} game s, {dist:7.0f} units ({dist / dt_game if dt_game > 0 else 0:6.0f} u/s), "
                f"highest +{zmax[6] - r0[6]:6.1f} after {zmax[2] - r0[2]:.3f} s")
        if down and zmax[6] - r0[6] > 5:
            line += f", back down after {down[2] - r0[2]:.3f} s"
        if len(r0) >= 10:                    # camera: turn around the player (yaw of camera -> player), distance
            yaw = lambda r: math.degrees(math.atan2(r[5] - r[8], r[4] - r[7]))
            turn = 0.0
            for a, b in zip(seg, seg[1:]):
                d = yaw(b) - yaw(a)
                turn += (d + 180) % 360 - 180
            dist0 = math.dist(r0[4:7], r0[7:10])
            dist1 = math.dist(seg[-1][4:7], seg[-1][7:10])
            line += f"; camera turned {turn:+6.1f} deg ({turn / dt_game if dt_game > 0 else 0:+6.1f} deg/s), distance {dist0:.0f} -> {dist1:.0f}"
        print(line)


def main():
    for d in sys.argv[1:]:
        report(d)


if __name__ == "__main__":
    sys.exit(main())
