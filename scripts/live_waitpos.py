"""Live play: wait until the player is near a world position (x,y) in a game started by live_game.ps1.

    python scripts/live_waitpos.py [--dir build/live] [--tol 60] [--timeout 600] [--stable 3] [--far] -- <x,y>

Reads the player focus from PS2_PEEK 2DE1C0 lines in <dir>/run.log (like live_goto.py); exits 0 and
prints "v<vblank> at x,y" once `stable` consecutive peeks are within tol (--far: farther than tol,
e.g. a cutscene took the camera focus away), exits 1 on timeout.
"""
import argparse
import re
import struct
import sys
import time
from pathlib import Path

PEEK = re.compile(r"^\[peek\] v(\d+) 002DE1C0:((?: [0-9A-F]{8}){3,})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("point")
    ap.add_argument("--dir", default=str(Path(__file__).resolve().parent.parent / "build" / "live"))
    ap.add_argument("--tol", type=float, default=60)
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--stable", type=int, default=3)
    ap.add_argument("--far", action="store_true")
    a = ap.parse_args()
    tx, ty = map(float, a.point.split(","))
    log = Path(a.dir) / "run.log"
    t0 = time.time()
    last = None
    while time.time() - t0 < a.timeout:
        good = 0
        for line in log.read_bytes()[-6000:].decode("latin-1").splitlines():
            m = PEEK.match(line)
            if not m:
                continue
            x, y = (struct.unpack("<f", struct.pack("<I", int(h, 16)))[0] for h in m.group(2).split()[:2])
            last = (int(m.group(1)), x, y)
            near = abs(x - tx) < a.tol and abs(y - ty) < a.tol
            good = good + 1 if near != a.far else 0
        if good >= a.stable:
            print("v%d at %.0f,%.0f" % last)
            return 0
        time.sleep(1)
    print("timeout; last", last)
    return 1


if __name__ == "__main__":
    sys.exit(main())
