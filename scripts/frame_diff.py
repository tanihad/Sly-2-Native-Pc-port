"""Frame-sequence differences (pc-smooth: menu shake, flicker): for consecutive PNGs (8-bit RGB, runtime/debug.c), the
share of pixels that changed, their bounding box, and the best whole-pixel shift (-3..3 in x and y) that maps the
previous frame onto this one in a region - a picture that moves shows a non-zero shift, one that flickers shows changes
without a shift. No dependencies.

    python scripts/frame_diff.py <frame.png>... [--region x,y,w,h] [--thr 8]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from contact_sheet import read_png  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames", nargs="+")
    ap.add_argument("--region")
    ap.add_argument("--thr", type=int, default=8)
    a = ap.parse_args()
    prev = None
    for f in a.frames:
        w, h, px = read_png(Path(f))
        if prev is None:
            prev = (w, h, px)
            print(f"{Path(f).name}: {w}x{h}")
            continue
        pw, ph, pp = prev
        rx, ry, rw, rh = (0, 0, w, h)
        if a.region:
            rx, ry, rw, rh = (int(v) for v in a.region.split(","))
        n = ch = 0
        x0, y0, x1, y1 = w, h, -1, -1
        for y in range(ry, min(ry + rh, h)):
            o = y * w * 3
            for x in range(rx, min(rx + rw, w)):
                i = o + x * 3
                d = abs(px[i] - pp[i]) + abs(px[i + 1] - pp[i + 1]) + abs(px[i + 2] - pp[i + 2])
                n += 1
                if d > a.thr:
                    ch += 1
                    x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
        best = None
        if ch:
            # whole-pixel shift on a subsampled grid of the region (inner part)
            for dy in range(-3, 4):
                for dx in range(-3, 4):
                    s = 0
                    for y in range(ry + 4, min(ry + rh, h) - 4, 3):
                        for x in range(rx + 4, min(rx + rw, w) - 4, 3):
                            i = (y * w + x) * 3
                            j = ((y - dy) * w + (x - dx)) * 3
                            s += abs(px[i] - pp[j]) + abs(px[i + 1] - pp[j + 1]) + abs(px[i + 2] - pp[j + 2])
                    if best is None or s < best[0]:
                        best = (s, dx, dy)
        print(f"{Path(f).name}: changed {100.0 * ch / max(1, n):.2f}%"
              + (f" in [{x0},{y0} - {x1},{y1}], best shift ({best[1]},{best[2]})" if ch else ""))
        prev = (w, h, px)


if __name__ == "__main__":
    main()
