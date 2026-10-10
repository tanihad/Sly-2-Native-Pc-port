"""Tile dumped frames (8-bit RGB PNGs from runtime/debug.c) into one contact sheet, no dependencies.

    python scripts/contact_sheet.py <out.png> <frame.png>... [--cols 3] [--scale 2]

Each frame is shrunk by --scale (box filter) and labelled by its position (row-major order);
frames that are missing are skipped. Handy for looking at many frames in one image.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path


def read_png(path: Path) -> tuple[int, int, bytearray]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat = 8, b""
    w = h = 0
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype != 2:
                raise ValueError(f"{path}: only 8-bit RGB supported")
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = 3 * w
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(3, stride): line[i] = (line[i] + line[i - 3]) & 255
        elif f == 2:
            for i in range(stride): line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride): line[i] = (line[i] + ((line[i - 3] if i >= 3 else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                b, c = prev[i], (prev[i - 3] if i >= 3 else 0)
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, out


def write_png(path: Path, w: int, h: int, rgb: bytes) -> None:
    raw = b"".join(b"\0" + rgb[y * 3 * w:(y + 1) * 3 * w] for y in range(h))
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def shrink(w: int, h: int, rgb: bytearray, s: int) -> tuple[int, int, bytearray]:
    if s == 1:
        return w, h, rgb
    nw, nh = w // s, h // s
    out = bytearray(nw * nh * 3)
    for y in range(nh):
        for x in range(nw):
            for c in range(3):
                t = 0
                for dy in range(s):
                    row = (y * s + dy) * w * 3
                    for dx in range(s):
                        t += rgb[row + (x * s + dx) * 3 + c]
                out[(y * nw + x) * 3 + c] = t // (s * s)
    return nw, nh, out


def crop(w: int, h: int, rgb: bytearray, box: tuple[int, int, int, int], zoom: int) -> tuple[int, int, bytearray]:
    x0, y0, x1, y1 = max(0, box[0]), max(0, box[1]), min(w, box[2]), min(h, box[3])
    nw, nh = (x1 - x0) * zoom, (y1 - y0) * zoom
    out = bytearray(nw * nh * 3)
    for y in range(nh):
        sy = y0 + y // zoom
        for x in range(nw):
            s = (sy * w + x0 + x // zoom) * 3
            out[(y * nw + x) * 3:(y * nw + x) * 3 + 3] = rgb[s:s + 3]
    return nw, nh, out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("frames", nargs="+")
    ap.add_argument("--cols", type=int, default=3)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--crop", help="x0,y0,x1,y1: cut this rectangle out of each frame first")
    ap.add_argument("--zoom", type=int, default=1, help="enlarge the crop (nearest neighbour)")
    a = ap.parse_args()
    tiles = []
    for f in a.frames:
        p = Path(f)
        if p.exists():
            img = read_png(p)
            if a.crop:
                img = crop(*img, tuple(int(v) for v in a.crop.split(",")), a.zoom)
            tiles.append(shrink(*img, a.scale))
    if not tiles:
        print("no frames")
        return 1
    tw = max(t[0] for t in tiles); th = max(t[1] for t in tiles)
    cols = min(a.cols, len(tiles)); rows = (len(tiles) + cols - 1) // cols
    W, H = cols * tw + (cols - 1) * 2, rows * th + (rows - 1) * 2
    sheet = bytearray(b"\xff" * (W * H * 3))
    for i, (w, h, rgb) in enumerate(tiles):
        ox, oy = (i % cols) * (tw + 2), (i // cols) * (th + 2)
        for y in range(h):
            d = ((oy + y) * W + ox) * 3
            sheet[d:d + w * 3] = rgb[y * w * 3:(y + 1) * w * 3]
    write_png(Path(a.out), W, H, bytes(sheet))
    print(f"{a.out}: {len(tiles)} frames, {W}x{H}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
