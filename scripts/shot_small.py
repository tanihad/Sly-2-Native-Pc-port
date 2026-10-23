"""Recompress (and optionally shrink) PNG screenshots written by the game (runtime/debug.c writes them uncompressed).

    python scripts/shot_small.py <out.png> <in.png> [--scale N]     (N: box-filter shrink factor, default 1)
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from contact_sheet import read_png  # noqa: E402
from menu_shots import shrink, write_png  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("inp")
    ap.add_argument("--scale", type=int, default=1)
    a = ap.parse_args()
    w, h, rgb = read_png(Path(a.inp))
    if a.scale > 1:
        rgb, w, h = shrink(rgb, w, h, a.scale)
    write_png(Path(a.out), w, h, rgb)
    print(f"{a.out}: {w}x{h}, {Path(a.out).stat().st_size // 1024} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
