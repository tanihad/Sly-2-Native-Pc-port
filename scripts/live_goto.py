"""Live play autopilot: steer the player to world positions in a game started by live_game.ps1.

    python scripts/live_goto.py <x,y>[ <x,y> ...] [--dir build/live] [--tol 80] [--sign 1] [--timeout 60]

Needs PS2_PEEK="2DE1C0:4,2DDFD0:4" PS2_PEEK_EVERY=<n> in the game's environment (Sly 2: 0x2DE1C0 is
the player position (125 above the floor), 0x2DDFD0 the camera position). The left stick is
camera-relative, so each step points it at the waypoint relative to that view direction. Prints the
position after each step; gives up on a waypoint when the player stops making progress.
"""
import argparse
import math
import re
import struct
import time
from pathlib import Path

PEEK = re.compile(r"^\[peek\] v(\d+) ([0-9A-F]{8}):((?: [0-9A-F]{8})+)")


def f32(h: str) -> float:
    return struct.unpack("<f", struct.pack("<I", int(h, 16)))[0]


def read_pose(log: Path):
    data = log.read_bytes()[-20000:].decode("latin-1").splitlines()
    pos = ahead = None
    vb = 0
    for line in data:
        m = PEEK.match(line)
        if not m:
            continue
        vals = [f32(w) for w in m.group(3).split()]
        if len(vals) < 3:                    # a line still being written
            continue
        if m.group(2) == "002DE1C0":
            pos, vb = vals, int(m.group(1))
        elif m.group(2) == "002DDFD0":
            ahead = vals
    return vb, pos, ahead


def send(inp: Path, text: str) -> None:
    with inp.open("a", encoding="ascii") as f:
        f.write(text + "\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("points", nargs="+")
    ap.add_argument("--dir", default=str(Path(__file__).resolve().parent.parent / "build" / "live"))
    ap.add_argument("--tol", type=float, default=80.0)
    ap.add_argument("--sign", type=float, default=1.0, help="flip if steering goes the wrong way")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--step", type=int, default=20, help="frames per stick command")
    a = ap.parse_args()
    d = Path(a.dir)
    log, inp = d / "run.log", d / "input.txt"
    for pt in a.points:
        tx, ty = map(float, pt.split(","))
        t0, best, stall = time.time(), 1e30, 0
        last_vb = -1
        while time.time() - t0 < a.timeout:
            vb, pos, ahead = read_pose(log)
            if pos is None or ahead is None:
                time.sleep(0.5)
                continue
            if vb == last_vb:
                time.sleep(0.1)
                continue
            last_vb = vb
            dx, dy = tx - pos[0], ty - pos[1]
            dist = math.hypot(dx, dy)
            print(f"v{vb} at {pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f} -> {tx:.0f},{ty:.0f}: {dist:.0f}", flush=True)
            if dist < a.tol:
                break
            if dist < best - 20:
                best, stall = dist, 0
            else:
                stall += 1
                if stall > 8:
                    print("no progress, giving up on this point")
                    break
            fx, fy = pos[0] - ahead[0], pos[1] - ahead[1]          # camera -> player
            fn = math.hypot(fx, fy) or 1.0
            fx, fy = fx / fn, fy / fn
            ang = math.atan2(fx * dy - fy * dx, fx * dx + fy * dy)   # + = target to the left
            sx = -math.sin(ang) * a.sign                               # stick right = +
            sy = -math.cos(ang)                                        # stick up = -
            hx = max(0, min(255, int(round(128 + sx * 127))))
            hy = max(0, min(255, int(round(128 + sy * 127))))
            send(inp, f"+0:lx{hx:02x}:{a.step},+0:ly{hy:02x}:{a.step}")
            time.sleep(0.25)
        else:
            print("timeout")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
