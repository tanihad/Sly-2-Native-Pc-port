"""Side-by-side crops of composed pictures at different scales (M7 contact sheets), no dependencies.

    python scripts/m7_sheet.py <out.png> --box x0,y0,x1,y1 [--zoom 3] [--native-w 512 --native-h 448] <picture.png>...

The box is in the native frame (the 1x picture's pixels: 512 x 448 for Sly's field display). Each picture's
scale is its width / --native-w; its crop is the box times that scale, shown at --zoom output pixels per native
pixel (nearest-neighbour enlargement, box-filter reduction), so a 1x and a 3x picture line up pixel for pixel.
Tiles are placed left to right with a 4-pixel white gap; --cols starts new rows.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from contact_sheet import read_png, write_png  # noqa: E402


def tile(path: Path, box, zoom: int, native_w: int, native_h: int):
    w, h, rgb = read_png(path)
    s, sy = max(1, w // native_w), max(1, h // native_h)
    x0, y0, x1, y1 = (box[0] * s, box[1] * sy, box[2] * s, box[3] * sy)
    tw, th = (box[2] - box[0]) * zoom, (box[3] - box[1]) * zoom
    out = bytearray(tw * th * 3)
    for v in range(th):
        for u in range(tw):
            # source area of output pixel (u, v): [x0 + u * s / zoom, x0 + (u + 1) * s / zoom)
            ax, bx = x0 + (u * s) // zoom, x0 + max(((u + 1) * s) // zoom, (u * s) // zoom + 1)
            ay, by = y0 + (v * sy) // zoom, y0 + max(((v + 1) * sy) // zoom, (v * sy) // zoom + 1)
            acc = [0, 0, 0]
            n = 0
            for yy in range(ay, min(by, h)):
                row = yy * w * 3
                for xx in range(ax, min(bx, w)):
                    p = row + xx * 3
                    acc[0] += rgb[p]; acc[1] += rgb[p + 1]; acc[2] += rgb[p + 2]
                    n += 1
            if n:
                d = (v * tw + u) * 3
                out[d:d + 3] = bytes((acc[0] // n, acc[1] // n, acc[2] // n))
    return tw, th, out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("pictures", nargs="+")
    ap.add_argument("--box", required=True)
    ap.add_argument("--zoom", type=int, default=3)
    ap.add_argument("--native-w", type=int, default=512)
    ap.add_argument("--native-h", type=int, default=448)
    ap.add_argument("--cols", type=int, default=0)
    a = ap.parse_args()
    box = tuple(int(v) for v in a.box.split(","))
    tiles = [tile(Path(p), box, a.zoom, a.native_w, a.native_h) for p in a.pictures]
    cols = a.cols or len(tiles)
    rows = (len(tiles) + cols - 1) // cols
    tw, th = tiles[0][0], tiles[0][1]
    W, H = cols * tw + (cols - 1) * 4, rows * th + (rows - 1) * 4
    sheet = bytearray(b"\xff" * (W * H * 3))
    for i, (w, h, rgb) in enumerate(tiles):
        ox, oy = (i % cols) * (tw + 4), (i // cols) * (th + 4)
        for y in range(h):
            d = ((oy + y) * W + ox) * 3
            sheet[d:d + w * 3] = rgb[y * w * 3:(y + 1) * w * 3]
    write_png(Path(a.out), W, H, bytes(sheet))
    print(f"{a.out}: {len(tiles)} tiles, {W}x{H}, {Path(a.out).stat().st_size // 1024} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
