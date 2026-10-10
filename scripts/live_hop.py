"""Live play: jump towards a world position (x,y) with the left stick held towards it.

    python scripts/live_hop.py <x,y> [--hold 25] [--jumps 1] [--gap 40] [--extra "<more tokens>"]

Uses the same pose source as live_goto.py (PS2_PEEK 2DE1C0 and 2DDFD0). The stick is held for
--hold frames and cross is tapped --jumps times, --gap frames apart, starting 2 frames in.
"""
import argparse
import math
import time
from pathlib import Path

import live_goto as lg


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("point")
    ap.add_argument("--dir", default=str(Path(__file__).resolve().parent.parent / "build" / "live"))
    ap.add_argument("--hold", type=int, default=25)
    ap.add_argument("--jumps", type=int, default=1)
    ap.add_argument("--gap", type=int, default=40)
    ap.add_argument("--scale", type=float, default=1.0, help="stick deflection 0..1")
    ap.add_argument("--extra", default="")
    a = ap.parse_args()
    d = Path(a.dir)
    tx, ty = map(float, a.point.split(","))
    vb, pos, cam = lg.read_pose(d / "run.log")
    fx, fy = pos[0] - cam[0], pos[1] - cam[1]
    n = math.hypot(fx, fy) or 1.0
    fx, fy = fx / n, fy / n
    dx, dy = tx - pos[0], ty - pos[1]
    ang = math.atan2(fx * dy - fy * dx, fx * dx + fy * dy)
    hx = max(0, min(255, int(round(128 - math.sin(ang) * 127 * a.scale))))
    hy = max(0, min(255, int(round(128 - math.cos(ang) * 127 * a.scale))))
    toks = [f"+0:lx{hx:02x}:{a.hold}", f"+0:ly{hy:02x}:{a.hold}", "+2:cross:3"]
    toks += [f"+{a.gap}:cross:3"] * (a.jumps - 1)
    if a.extra:
        toks.append(a.extra)
    lg.send(d / "input.txt", ",".join(toks))
    print(f"v{vb} at {pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f}: hop towards {tx:.0f},{ty:.0f} "
          f"({math.hypot(dx, dy):.0f} away), stick {hx:02x},{hy:02x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
