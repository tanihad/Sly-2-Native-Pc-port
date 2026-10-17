"""Episode contact sheet for the GPU renderer's in-game sweep (M8, scripts/m8_sweep.ps1), no dependencies.

    python scripts/m8_sheet.py <out.png> --row <sw.png> <hw.png> [x0,y0] --row ... [--width 256] [--crop 96x84] [--zoom 3]

One row per scene, four tiles: the software renderer's picture (native, e.g. pair_<v>_sw.png from the in-game
verifier) and the GPU renderer's picture of the same VBlank (frame_<v>.png, at internal scale), both reduced to
--width pixels (box filter); then a crop of --crop native pixels at native position x0,y0 (default: the centre) from
each, at --zoom output pixels per native pixel (the 1x picture nearest-enlarged, the 3x picture 1:1 at zoom 3).
A picture's scale is its size over the sw picture's. Tiles are separated by 4 white pixels.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from contact_sheet import read_png, write_png  # noqa: E402


def area(w, h, rgb, x0, y0, x1, y1, tw, th):
    """Source rectangle [x0, x1) x [y0, y1) (float source pixels) resampled to tw x th: box average, or nearest when enlarging."""
    out = bytearray(tw * th * 3)
    sx, sy = (x1 - x0) / tw, (y1 - y0) / th
    for v in range(th):
        ay = int(y0 + v * sy)
        by = max(ay + 1, int(y0 + (v + 1) * sy))
        by = min(by, h)
        for u in range(tw):
            ax = int(x0 + u * sx)
            bx = min(max(ax + 1, int(x0 + (u + 1) * sx)), w)
            r = g = b = n = 0
            for yy in range(ay, by):
                p = (yy * w + ax) * 3
                for _ in range(ax, bx):
                    r += rgb[p]; g += rgb[p + 1]; b += rgb[p + 2]
                    p += 3
                    n += 1
            if n:
                d = (v * tw + u) * 3
                out[d] = r // n; out[d + 1] = g // n; out[d + 2] = b // n
    return out


def write_png_up(path: Path, w: int, h: int, rgb: bytearray) -> None:
    """8-bit RGB PNG, each row with the PNG filter (Sub / Up / Paeth) of the smallest sum of absolute residuals."""
    import struct
    import zlib
    stride = w * 3
    rows = []
    prev = bytes(stride)
    for y in range(h):
        line = bytes(rgb[y * stride:(y + 1) * stride])
        left = bytes(3) + line[:-3]
        upleft = bytes(3) + prev[:-3]
        sub = bytes((a - b) & 255 for a, b in zip(line, left))
        up = bytes((a - b) & 255 for a, b in zip(line, prev))
        pa = bytearray(stride)
        for i in range(stride):
            a_, b_, c_ = left[i], prev[i], upleft[i]
            p = a_ + b_ - c_
            da, db, dc = abs(p - a_), abs(p - b_), abs(p - c_)
            pa[i] = (line[i] - (a_ if da <= db and da <= dc else b_ if db <= dc else c_)) & 255
        best = min(((1, sub), (2, up), (4, bytes(pa))), key=lambda t: sum(v if v < 128 else 256 - v for v in t[1]))
        rows.append(bytes((best[0],)) + best[1])
        prev = line

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--row", nargs="+", action="append", required=True, metavar="ARG")
    ap.add_argument("--width", type=int, default=256)
    ap.add_argument("--crop", default="96x84")
    ap.add_argument("--zoom", type=int, default=3)
    a = ap.parse_args()
    cw, ch = (int(v) for v in a.crop.split("x"))
    rows = []
    for spec in a.row:
        sw_w, sw_h, sw = read_png(Path(spec[0]))
        hw_w, hw_h, hw = read_png(Path(spec[1]))
        if len(spec) > 2:
            cx, cy = (int(v) for v in spec[2].split(","))
        else:
            cx, cy = (sw_w - cw) // 2, (sw_h - ch) // 2
        kx, ky = hw_w / sw_w, hw_h / sw_h
        th = a.width * sw_h // sw_w
        tiles = [
            (a.width, th, area(sw_w, sw_h, sw, 0, 0, sw_w, sw_h, a.width, th)),
            (a.width, th, area(hw_w, hw_h, hw, 0, 0, hw_w, hw_h, a.width, th)),
            (cw * a.zoom, ch * a.zoom, area(sw_w, sw_h, sw, cx, cy, cx + cw, cy + ch, cw * a.zoom, ch * a.zoom)),
            (cw * a.zoom, ch * a.zoom, area(hw_w, hw_h, hw, cx * kx, cy * ky, (cx + cw) * kx, (cy + ch) * ky, cw * a.zoom, ch * a.zoom)),
        ]
        rows.append(tiles)
        print(f"{spec[1]}: {hw_w}x{hw_h} vs sw {sw_w}x{sw_h}, crop at {cx},{cy}")
    W = max(sum(t[0] for t in r) + 4 * (len(r) - 1) for r in rows)
    hs = [max(t[1] for t in r) for r in rows]
    H = sum(hs) + 4 * (len(rows) - 1)
    sheet = bytearray(b"\xff" * (W * H * 3))
    y = 0
    for r, rh in zip(rows, hs):
        x = 0
        for tw, th, rgb in r:
            for v in range(th):
                d = ((y + v) * W + x) * 3
                sheet[d:d + tw * 3] = rgb[v * tw * 3:(v + 1) * tw * 3]
            x += tw + 4
        y += rh + 4
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    write_png_up(Path(a.out), W, H, sheet)
    print(f"{a.out}: {W}x{H}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
